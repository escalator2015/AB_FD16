/*
 * AudioBus — FD16 fixed point-to-point full-duplex profile core
 *
 * A dedicated, discovery-free execution path. No slot-map negotiation, no
 * direction switching, no 8b10b. Both TX and RX operate simultaneously.
 *
 * Data flow:
 *   TX: app → abus_fd16_audio_write() → TX ring buffer → frame task packs
 *       72-byte frame (abus_fd16_pack) → PHY tx_frame → PARLIO DMA.
 *   RX: PARLIO RX ISR → PHY rx callback → RX queue → frame task unpacks
 *       (abus_fd16_unpack) → CRC/sequence check → RX ring buffer → app.
 *
 * Error handling:
 *   - CRC/sync failure: drop frame, repeat last valid samples (concealment),
 *     increment counters.
 *   - Sequence gap: accept frame, increment sequence_errors.
 *   - Consecutive bad frames >= threshold: mute output (ramp to zero).
 *
 * SPDX-License-Identifier: MIT
 */

#include "audiobus_fd16.h"
#include "audiobus_phy.h"
#include "fd16_frame.h"

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

#include <string.h>

static const char *TAG = "abus_fd16";

/* ---------------------------------------------------------------------------
 * Internal handle
 * --------------------------------------------------------------------------- */

struct abus_fd16_handle {
    abus_fd16_config_t config;
    abus_phy_t         phy;

    /* TX/RX ring buffers (16 channels of int32_t) */
    int32_t *tx_ring;
    int32_t *rx_ring;
    uint16_t ring_frames;
    volatile uint16_t tx_write;
    volatile uint16_t tx_read;
    volatile uint16_t rx_write;
    volatile uint16_t rx_read;

    /* Last valid samples (for repeat-last-sample concealment) */
    int32_t last_valid[ABUS_FD16_CHANNELS];
    bool     have_valid;

    /* Sequence tracking */
    uint16_t tx_sequence;
    uint16_t last_rx_seq;
    bool     have_rx_seq;

    /* Frame task */
    TaskHandle_t task_handle;
    QueueHandle_t rx_queue;

    /* Statistics */
    abus_fd16_stats_t stats;
};

/* ---------------------------------------------------------------------------
 * Ring buffer helpers (16 channels fixed)
 * --------------------------------------------------------------------------- */

static inline uint16_t ring_available(uint16_t w, uint16_t r, uint16_t n) {
    int diff = (int)w - (int)r;
    if (diff < 0) diff += n;
    return (uint16_t)diff;
}

static inline uint16_t ring_space(uint16_t w, uint16_t r, uint16_t n) {
    return n - 1 - ring_available(w, r, n);
}

/* ---------------------------------------------------------------------------
 * PHY RX callback — called from ISR, pushes frame to processing queue
 * --------------------------------------------------------------------------- */

typedef struct {
    uint8_t data[ABUS_FD16_FRAME_BYTES];
} fd16_rx_msg_t;

static void fd16_phy_rx_cb(uint8_t port, const uint8_t *data,
                           uint16_t len, void *arg) {
    struct abus_fd16_handle *h = (struct abus_fd16_handle *)arg;
    if (!h || !data || len != ABUS_FD16_FRAME_BYTES) return;

    /* Copy into a pre-allocated message (no heap in ISR) */
    fd16_rx_msg_t msg;
    memcpy(msg.data, data, ABUS_FD16_FRAME_BYTES);

    if (xQueueSendFromISR(h->rx_queue, &msg, NULL) != pdTRUE) {
        h->stats.rx_overruns++;
    }
}

/* ---------------------------------------------------------------------------
 * Frame task — packs TX and unpacks RX at the sample rate
 * --------------------------------------------------------------------------- */

static void fd16_frame_task(void *arg) {
    struct abus_fd16_handle *h = (struct abus_fd16_handle *)arg;
    fd16_rx_msg_t msg;
    uint8_t frame[ABUS_FD16_FRAME_BYTES];

    while (1) {
        /* --- TX: pack one frame from the TX ring buffer --- */
        int32_t tx_samples[ABUS_FD16_CHANNELS];

        if (ring_available(h->tx_write, h->tx_read, h->ring_frames) > 0) {
            memcpy(tx_samples, &h->tx_ring[h->tx_read * ABUS_FD16_CHANNELS],
                   ABUS_FD16_AUDIO_BYTES);
            h->tx_read = (h->tx_read + 1) % h->ring_frames;
        } else {
            memset(tx_samples, 0, sizeof(tx_samples));
            h->stats.tx_underruns++;
        }

        abus_fd16_pack(frame, h->tx_sequence++, tx_samples);
        h->phy.ops->tx_frame(h->phy.ctx, 0, frame, ABUS_FD16_FRAME_BYTES);
        h->stats.tx_frames++;

        /* --- RX: process one received frame (blocking with timeout) --- */
        if (xQueueReceive(h->rx_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            int32_t rx_samples[ABUS_FD16_CHANNELS];
            uint16_t seq = 0;

            if (!abus_fd16_check_sync(msg.data)) {
                h->stats.sync_errors++;
                h->stats.consecutive_errors++;
                goto conceal;
            }

            if (!abus_fd16_unpack(msg.data, &seq, rx_samples)) {
                h->stats.crc_errors++;
                h->stats.consecutive_errors++;
                goto conceal;
            }

            /* Valid frame */
            h->stats.rx_frames++;
            h->stats.consecutive_errors = 0;

            /* Sequence check */
            if (h->have_rx_seq) {
                uint16_t expected = h->last_rx_seq + 1;
                if (seq != expected) {
                    h->stats.sequence_errors++;
                }
            }
            h->last_rx_seq = seq;
            h->have_rx_seq = true;
            h->stats.last_seq = seq;

            /* Store as last-valid for concealment */
            memcpy(h->last_valid, rx_samples, ABUS_FD16_AUDIO_BYTES);
            h->have_valid = true;

            /* Write to RX ring buffer */
            if (ring_space(h->rx_write, h->rx_read, h->ring_frames) > 0) {
                memcpy(&h->rx_ring[h->rx_write * ABUS_FD16_CHANNELS],
                       rx_samples, ABUS_FD16_AUDIO_BYTES);
                h->rx_write = (h->rx_write + 1) % h->ring_frames;
            } else {
                h->stats.rx_overruns++;
            }
            continue;

conceal:
            /* Repeat last valid samples (concealment) */
            if (h->have_valid) {
                if (ring_space(h->rx_write, h->rx_read, h->ring_frames) > 0) {
                    memcpy(&h->rx_ring[h->rx_write * ABUS_FD16_CHANNELS],
                           h->last_valid, ABUS_FD16_AUDIO_BYTES);
                    h->rx_write = (h->rx_write + 1) % h->ring_frames;
                }
            }

            /* Mute if consecutive errors exceed threshold */
            if (h->config.concealment_threshold > 0 &&
                h->stats.consecutive_errors >= h->config.concealment_threshold) {
                /* Ramp output to zero: overwrite last_valid with silence */
                memset(h->last_valid, 0, ABUS_FD16_AUDIO_BYTES);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

esp_err_t abus_fd16_init(const abus_fd16_config_t *config, abus_fd16_handle_t *out_handle) {
    ESP_RETURN_ON_FALSE(config && out_handle, ESP_ERR_INVALID_ARG, TAG, "null arg");

    struct abus_fd16_handle *h = calloc(1, sizeof(struct abus_fd16_handle));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "alloc handle");

    h->config = *config;
    h->ring_frames = config->audio_buffer_frames ? config->audio_buffer_frames : 8;

    /* Allocate ring buffers */
    h->tx_ring = heap_caps_calloc(h->ring_frames * ABUS_FD16_CHANNELS,
                                  sizeof(int32_t), MALLOC_CAP_INTERNAL);
    h->rx_ring = heap_caps_calloc(h->ring_frames * ABUS_FD16_CHANNELS,
                                  sizeof(int32_t), MALLOC_CAP_INTERNAL);
    if (!h->tx_ring || !h->rx_ring) {
        heap_caps_free(h->tx_ring);
        heap_caps_free(h->rx_ring);
        free(h);
        return ESP_ERR_NO_MEM;
    }

    /* Create PHY */
    esp_err_t ret = abus_phy_fd16_create(&config->pins, config->role, &h->phy);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "FD16 PHY create failed");

    ret = h->phy.ops->init(h->phy.ctx);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "FD16 PHY init failed");

    /* Register RX callback */
    h->phy.ops->set_rx_callback(h->phy.ctx, 0, fd16_phy_rx_cb, h);

    /* Create RX queue */
    h->rx_queue = xQueueCreate(8, sizeof(fd16_rx_msg_t));
    if (!h->rx_queue) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    *out_handle = h;
    ESP_LOGI(TAG, "FD16 initialized: role=%s, 16ch/48kHz/32-bit",
             config->role == ABUS_ROLE_MASTER ? "master" : "slave");
    return ESP_OK;

fail:
    if (h->phy.ctx) h->phy.ops->deinit(h->phy.ctx);
    heap_caps_free(h->tx_ring);
    heap_caps_free(h->rx_ring);
    free(h);
    return ret;
}

esp_err_t abus_fd16_start(abus_fd16_handle_t handle) {
    struct abus_fd16_handle *h = handle;
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");

    esp_err_t ret = h->phy.ops->start(h->phy.ctx);
    ESP_RETURN_ON_ERROR(ret, TAG, "FD16 PHY start failed");

    xTaskCreatePinnedToCore(fd16_frame_task, "fd16_frame", 4096, h,
                            configMAX_PRIORITIES - 2, &h->task_handle, 1);

    ESP_LOGI(TAG, "FD16 started (full-duplex)");
    return ESP_OK;
}

esp_err_t abus_fd16_stop(abus_fd16_handle_t handle) {
    struct abus_fd16_handle *h = handle;
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");

    if (h->task_handle) {
        vTaskDelete(h->task_handle);
        h->task_handle = NULL;
    }

    h->phy.ops->stop(h->phy.ctx);
    ESP_LOGI(TAG, "FD16 stopped");
    return ESP_OK;
}

esp_err_t abus_fd16_deinit(abus_fd16_handle_t handle) {
    struct abus_fd16_handle *h = handle;
    if (!h) return ESP_OK;

    abus_fd16_stop(h);
    h->phy.ops->deinit(h->phy.ctx);

    if (h->rx_queue) vQueueDelete(h->rx_queue);
    heap_caps_free(h->tx_ring);
    heap_caps_free(h->rx_ring);
    free(h);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Audio I/O
 * --------------------------------------------------------------------------- */

int abus_fd16_audio_write(abus_fd16_handle_t handle, const int32_t samples[ABUS_FD16_CHANNELS]) {
    struct abus_fd16_handle *h = handle;
    if (!h || !samples) return 0;

    if (ring_space(h->tx_write, h->tx_read, h->ring_frames) == 0) {
        return 0;
    }

    memcpy(&h->tx_ring[h->tx_write * ABUS_FD16_CHANNELS],
           samples, ABUS_FD16_AUDIO_BYTES);
    h->tx_write = (h->tx_write + 1) % h->ring_frames;
    return 1;
}

int abus_fd16_audio_read(abus_fd16_handle_t handle, int32_t samples[ABUS_FD16_CHANNELS]) {
    struct abus_fd16_handle *h = handle;
    if (!h || !samples) return 0;

    if (ring_available(h->rx_write, h->rx_read, h->ring_frames) == 0) {
        return 0;
    }

    memcpy(samples, &h->rx_ring[h->rx_read * ABUS_FD16_CHANNELS],
           ABUS_FD16_AUDIO_BYTES);
    h->rx_read = (h->rx_read + 1) % h->ring_frames;
    return 1;
}

int abus_fd16_audio_available(abus_fd16_handle_t handle) {
    struct abus_fd16_handle *h = handle;
    if (!h) return 0;
    return ring_available(h->rx_write, h->rx_read, h->ring_frames);
}

/* ---------------------------------------------------------------------------
 * Status
 * --------------------------------------------------------------------------- */

esp_err_t abus_fd16_get_stats(abus_fd16_handle_t handle, abus_fd16_stats_t *out_stats) {
    struct abus_fd16_handle *h = handle;
    ESP_RETURN_ON_FALSE(h && out_stats, ESP_ERR_INVALID_ARG, TAG, "null arg");
    *out_stats = h->stats;
    return ESP_OK;
}

bool abus_fd16_link_up(abus_fd16_handle_t handle) {
    struct abus_fd16_handle *h = handle;
    if (!h) return false;
    return h->stats.rx_frames > 0;
}
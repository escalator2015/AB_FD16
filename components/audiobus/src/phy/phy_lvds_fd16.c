/*
 * AudioBus — FD16 full-duplex fixed point-to-point PHY driver
 *
 * Uses the SN65LVDS049 (TI) — a dual LVDS transceiver (two drivers + two
 * receivers). Full-duplex: one pair for DATA A->B, one pair for DATA B->A,
 * one pair for CLOCK A->B (27.648 MHz), one pair for reference/shield.
 *
 * No 8b10b, no CDR, no turnaround, no direction switching. Both drivers and
 * receivers remain enabled at all times. Fixed 72-byte frame per sample period.
 *
 * Clocking:
 *   Node A (master): Si5351A generates 27.648 MHz → local PARLIO (TX+RX) ext
 *                    clock AND SN65LVDS049 DIN2 → DOUT2 → Cat6A Pair 3 → Node B.
 *   Node B (slave):  receives clock on RIN2 → ROUT2 → local PARLIO (TX+RX) ext
 *                    clock. B's TX and RX are phase-locked to A with zero offset.
 *
 * PARLIO:
 *   TX: 1-bit data @ 27.648 MHz, external clock, 576 bits (72 bytes) per frame.
 *   RX: 1-bit data @ 27.648 MHz, external clock (free-running), soft delimiter
 *       with eof_data_len = 72 bytes.
 *
 * RX re-queue strategy (ESP-IDF v5.5.4):
 *   `parlio_rx_unit_receive()` takes an internal mutex and is NOT ISR-safe.
 *   Therefore the RX done ISR only copies the 72-byte frame into a static
 *   buffer, invokes the upper-layer callback, and signals a semaphore. A
 *   dedicated re-queue task then calls `parlio_rx_unit_receive()` to re-arm
 *   the finished ping-pong buffer. Two RX buffers are kept in flight so the
 *   free-running clock never stalls (the driver auto-advances to the next
 *   queued transaction on EOF).
 *
 * SPDX-License-Identifier: MIT
 */

#include "audiobus_phy.h"
#include "audiobus_types.h"
#include "fd16_frame.h"
#include "si5351a.h"

#include "driver/parlio_tx.h"
#include "driver/parlio_rx.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "abus_phy_fd16";

#define DMA_ALIGN       4
#define DMA_BUF_ATTR    (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)

/* Fixed 72-byte frame, 576 bits */
#define FD16_FRAME_BITS (ABUS_FD16_FRAME_BYTES * 8)

/* ---------------------------------------------------------------------------
 * FD16 PHY context
 * --------------------------------------------------------------------------- */

typedef struct {
    abus_role_t role;

    /* GPIO assignments */
    int8_t tx_data_pin;     /* PARLIO TX data[0] → SN65LVDS049 DIN1 */
    int8_t rx_data_pin;     /* SN65LVDS049 ROUT1 → PARLIO RX data[0] */
    int8_t tx_clk_pin;      /* PARLIO TX clock output (optional) */
    int8_t clk_in_pin;      /* 27.648 MHz external clock (TX + RX) */
    int8_t clk_out_pin;     /* SN65LVDS049 DIN2 (clock driver, master only) */
    int8_t i2c_sda_pin;     /* Si5351A I2C SDA (master only) */
    int8_t i2c_scl_pin;     /* Si5351A I2C SCL (master only) */

    /* PARLIO handles */
    parlio_tx_unit_handle_t     tx_unit;
    parlio_rx_unit_handle_t     rx_unit;
    parlio_rx_delimiter_handle_t rx_delimiter;

    /* Fixed-size DMA buffers — double buffered (ping-pong) */
    uint8_t *tx_buf[2];
    volatile uint8_t tx_buf_idx;
    uint8_t *rx_buf[2];
    volatile uint8_t rx_buf_idx;

    /* Static ISR-safe frame copy buffer (no heap in ISR) */
    uint8_t rx_frame_static[ABUS_FD16_FRAME_BYTES];

    /* Frame RX callback (set by upper layer, called from ISR) */
    void (*rx_frame_cb)(uint8_t port, const uint8_t *data, uint16_t len, void *arg);
    void *rx_frame_cb_arg;

    /* Sync */
    SemaphoreHandle_t tx_done_sem;
    SemaphoreHandle_t rx_done_sem;
    TaskHandle_t      requeue_task;
    volatile bool     running;

    /* Statistics */
    volatile uint32_t frames_tx;
    volatile uint32_t frames_rx;
    volatile uint32_t rx_overruns;
    volatile uint32_t tx_underruns;

} fd16_phy_ctx_t;

/* ---------------------------------------------------------------------------
 * ISR callbacks (IRAM-safe, no heap allocation)
 * --------------------------------------------------------------------------- */

static bool IRAM_ATTR fd16_tx_done_cb(parlio_tx_unit_handle_t unit,
                                       const parlio_tx_done_event_data_t *edata,
                                       void *user_data) {
    fd16_phy_ctx_t *ctx = (fd16_phy_ctx_t *)user_data;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(ctx->tx_done_sem, &hp);
    ctx->frames_tx++;
    return hp == pdTRUE;
}

static bool IRAM_ATTR fd16_rx_done_cb(parlio_rx_unit_handle_t unit,
                                       const parlio_rx_event_data_t *edata,
                                       void *user_data) {
    fd16_phy_ctx_t *ctx = (fd16_phy_ctx_t *)user_data;
    BaseType_t hp = pdFALSE;

    if (!edata->data) {
        ctx->rx_overruns++;
        return false;
    }

    /* Validate fixed-size frame */
    if (edata->recv_bytes != ABUS_FD16_FRAME_BYTES) {
        ctx->rx_overruns++;
    } else {
        /* Copy into static buffer (ISR-safe, no heap alloc) */
        memcpy(ctx->rx_frame_static, edata->data, ABUS_FD16_FRAME_BYTES);
        if (ctx->rx_frame_cb) {
            ctx->rx_frame_cb(0, ctx->rx_frame_static,
                             ABUS_FD16_FRAME_BYTES, ctx->rx_frame_cb_arg);
        }
        ctx->frames_rx++;
    }

    /* Signal the re-queue task to re-arm the finished buffer */
    xSemaphoreGiveFromISR(ctx->rx_done_sem, &hp);

    return hp == pdTRUE;
}

/* ---------------------------------------------------------------------------
 * RX re-queue task — re-arms the finished ping-pong buffer
 *
 * `parlio_rx_unit_receive()` is not ISR-safe (takes a mutex), so re-queueing
 * happens here in task context. Two buffers stay in flight: when one finishes,
 * the driver auto-advances to the other already-queued buffer, and this task
 * re-arms the finished one before the second completes.
 * --------------------------------------------------------------------------- */

static void fd16_requeue_task(void *arg) {
    fd16_phy_ctx_t *ctx = (fd16_phy_ctx_t *)arg;

    while (ctx->running) {
        if (xSemaphoreTake(ctx->rx_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        /* Re-arm the buffer that just finished (the inactive one) */
        uint8_t finished = ctx->rx_buf_idx ^ 1;
        parlio_receive_config_t rcfg = { .delimiter = ctx->rx_delimiter };
        esp_err_t ret = parlio_rx_unit_receive(ctx->rx_unit, ctx->rx_buf[finished],
                                               ABUS_FD16_FRAME_BYTES, &rcfg);
        if (ret != ESP_OK) {
            ctx->rx_overruns++;
        }
        ctx->rx_buf_idx = finished;
    }

    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * PHY ops implementation
 * --------------------------------------------------------------------------- */

static esp_err_t fd16_init(void *phy_ctx) {
    fd16_phy_ctx_t *ctx = (fd16_phy_ctx_t *)phy_ctx;

    /* Allocate fixed-size DMA buffers (ping-pong) */
    for (int i = 0; i < 2; i++) {
        ctx->tx_buf[i] = heap_caps_aligned_calloc(DMA_ALIGN, 1,
                                                  ABUS_FD16_FRAME_BYTES, DMA_BUF_ATTR);
        ctx->rx_buf[i] = heap_caps_aligned_calloc(DMA_ALIGN, 1,
                                                  ABUS_FD16_FRAME_BYTES, DMA_BUF_ATTR);
        if (!ctx->tx_buf[i] || !ctx->rx_buf[i]) {
            return ESP_ERR_NO_MEM;
        }
    }
    ctx->tx_buf_idx = 0;
    ctx->rx_buf_idx = 0;

    ctx->tx_done_sem = xSemaphoreCreateBinary();
    ctx->rx_done_sem = xSemaphoreCreateBinary();
    if (!ctx->tx_done_sem || !ctx->rx_done_sem) return ESP_ERR_NO_MEM;

    /* --- Si5351A clock source (master only) ---
     * Program the Si5351A to generate the 27.648 MHz transport clock on CLK0
     * BEFORE creating the PARLIO units, so the external clock is present when
     * PARLIO starts. The slave (B) has no Si5351A; it receives the clock. */
    if (ctx->role == ABUS_ROLE_MASTER &&
        ctx->i2c_sda_pin >= 0 && ctx->i2c_scl_pin >= 0) {
        abus_si5351a_config_t si = {
            .sda_pin = ctx->i2c_sda_pin,
            .scl_pin = ctx->i2c_scl_pin,
            .i2c_clk_hz = CONFIG_ABUS_FD16_I2C_CLK_HZ,
            .xtal_hz = CONFIG_ABUS_FD16_SI5351A_XTAL_HZ,
            .clk0_hz = CONFIG_ABUS_FD16_SI5351A_CLK0_HZ,
        };
        ESP_RETURN_ON_ERROR(abus_si5351a_init(&si), TAG, "Si5351A init failed");
    }

    /* --- PARLIO TX: 1-bit, external 27.648 MHz clock --- */
    gpio_num_t tx_data[PARLIO_TX_UNIT_MAX_DATA_WIDTH];
    memset(tx_data, -1, sizeof(tx_data));
    tx_data[0] = ctx->tx_data_pin;

    parlio_tx_unit_config_t tx_cfg = {
        .clk_src = PARLIO_CLK_SRC_EXTERNAL,
        .clk_in_gpio_num = ctx->clk_in_pin,
        .input_clk_src_freq_hz = ABUS_FD16_CLOCK_HZ,
        .output_clk_freq_hz = ABUS_FD16_CLOCK_HZ,   /* Divider = 1 */
        .data_width = 1,
        .clk_out_gpio_num = ctx->tx_clk_pin,
        .valid_gpio_num = -1,
        .trans_queue_depth = 4,
        .max_transfer_size = ABUS_FD16_FRAME_BYTES,
        .dma_burst_size = 64,
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_MSB,
    };
    memcpy(tx_cfg.data_gpio_nums, tx_data, sizeof(tx_data));

    esp_err_t ret = parlio_new_tx_unit(&tx_cfg, &ctx->tx_unit);
    ESP_RETURN_ON_ERROR(ret, TAG, "FD16 TX unit create failed");

    /* --- PARLIO RX: 1-bit, external 27.648 MHz free-running clock --- */
    gpio_num_t rx_data[PARLIO_RX_UNIT_MAX_DATA_WIDTH];
    memset(rx_data, -1, sizeof(rx_data));
    rx_data[0] = ctx->rx_data_pin;

    parlio_rx_unit_config_t rx_cfg = {
        .clk_src = PARLIO_CLK_SRC_EXTERNAL,
        .ext_clk_freq_hz = ABUS_FD16_CLOCK_HZ,
        .exp_clk_freq_hz = ABUS_FD16_CLOCK_HZ,
        .clk_in_gpio_num = ctx->clk_in_pin,
        .data_width = 1,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .trans_queue_depth = 4,
        .max_recv_size = ABUS_FD16_FRAME_BYTES,
        .dma_burst_size = 64,
        .flags.free_clk = 1,    /* Clock is free-running */
    };
    memcpy(rx_cfg.data_gpio_nums, rx_data, sizeof(rx_data));

    ret = parlio_new_rx_unit(&rx_cfg, &ctx->rx_unit);
    ESP_RETURN_ON_ERROR(ret, TAG, "FD16 RX unit create failed");

    /* --- Soft delimiter: fixed 72-byte frame --- */
    parlio_rx_soft_delimiter_config_t delim = {
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_MSB,
        .eof_data_len = ABUS_FD16_FRAME_BYTES,
        .timeout_ticks = ABUS_FD16_CLOCK_HZ / 1000,  /* 1 ms timeout */
    };
    ret = parlio_new_rx_soft_delimiter(&delim, &ctx->rx_delimiter);
    ESP_RETURN_ON_ERROR(ret, TAG, "FD16 RX delimiter create failed");

    /* --- Register ISR callbacks --- */
    parlio_tx_event_callbacks_t tx_cbs = { .on_trans_done = fd16_tx_done_cb };
    parlio_tx_unit_register_event_callbacks(ctx->tx_unit, &tx_cbs, ctx);

    parlio_rx_event_callbacks_t rx_cbs = { .on_receive_done = fd16_rx_done_cb };
    parlio_rx_unit_register_event_callbacks(ctx->rx_unit, &rx_cbs, ctx);

    ESP_LOGI(TAG, "FD16 PHY init: %lu Hz, 72-byte frames, role=%s",
             (unsigned long)ABUS_FD16_CLOCK_HZ,
             ctx->role == ABUS_ROLE_MASTER ? "master" : "slave");
    return ESP_OK;
}

static esp_err_t fd16_start(void *phy_ctx) {
    fd16_phy_ctx_t *ctx = (fd16_phy_ctx_t *)phy_ctx;
    ctx->running = true;

    ESP_RETURN_ON_ERROR(parlio_tx_unit_enable(ctx->tx_unit), TAG, "FD16 TX enable");
    ESP_RETURN_ON_ERROR(parlio_rx_unit_enable(ctx->rx_unit, true), TAG, "FD16 RX enable");

    /* Queue both RX buffers (ping-pong) so the free-running clock never stalls */
    parlio_receive_config_t rcfg = { .delimiter = ctx->rx_delimiter };
    ESP_RETURN_ON_ERROR(parlio_rx_unit_receive(ctx->rx_unit, ctx->rx_buf[0],
                        ABUS_FD16_FRAME_BYTES, &rcfg), TAG, "FD16 RX buf0");
    ESP_RETURN_ON_ERROR(parlio_rx_unit_receive(ctx->rx_unit, ctx->rx_buf[1],
                        ABUS_FD16_FRAME_BYTES, &rcfg), TAG, "FD16 RX buf1");
    parlio_rx_soft_delimiter_start_stop(ctx->rx_unit, ctx->rx_delimiter, true);

    /* Start the re-queue task */
    xTaskCreatePinnedToCore(fd16_requeue_task, "fd16_rxq", 2048, ctx,
                            configMAX_PRIORITIES - 3, &ctx->requeue_task, 1);

    ESP_LOGI(TAG, "FD16 PHY started (full-duplex)");
    return ESP_OK;
}

static esp_err_t fd16_stop(void *phy_ctx) {
    fd16_phy_ctx_t *ctx = (fd16_phy_ctx_t *)phy_ctx;
    ctx->running = false;

    parlio_rx_soft_delimiter_start_stop(ctx->rx_unit, ctx->rx_delimiter, false);
    parlio_tx_unit_disable(ctx->tx_unit);
    parlio_rx_unit_disable(ctx->rx_unit);

    if (ctx->requeue_task) {
        vTaskDelay(pdMS_TO_TICKS(50));
        ctx->requeue_task = NULL;
    }
    return ESP_OK;
}

static esp_err_t fd16_deinit(void *phy_ctx) {
    fd16_phy_ctx_t *ctx = (fd16_phy_ctx_t *)phy_ctx;

    if (ctx->tx_unit) parlio_del_tx_unit(ctx->tx_unit);
    if (ctx->rx_unit) parlio_del_rx_unit(ctx->rx_unit);
    if (ctx->rx_delimiter) parlio_del_rx_delimiter(ctx->rx_delimiter);
    if (ctx->tx_done_sem) vSemaphoreDelete(ctx->tx_done_sem);
    if (ctx->rx_done_sem) vSemaphoreDelete(ctx->rx_done_sem);

    for (int i = 0; i < 2; i++) {
        heap_caps_free(ctx->tx_buf[i]);
        heap_caps_free(ctx->rx_buf[i]);
    }
    free(ctx);
    return ESP_OK;
}

static esp_err_t fd16_tx_frame(void *phy_ctx, uint8_t port,
                               const uint8_t *data, uint16_t len) {
    fd16_phy_ctx_t *ctx = (fd16_phy_ctx_t *)phy_ctx;

    if (len != ABUS_FD16_FRAME_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Write to inactive buffer (ping-pong) */
    uint8_t idx = ctx->tx_buf_idx ^ 1;
    memcpy(ctx->tx_buf[idx], data, ABUS_FD16_FRAME_BYTES);

    parlio_transmit_config_t tcfg = { .idle_value = 0 };
    esp_err_t ret = parlio_tx_unit_transmit(ctx->tx_unit, ctx->tx_buf[idx],
                                            FD16_FRAME_BITS, &tcfg);
    if (ret == ESP_OK) {
        ctx->tx_buf_idx = idx;
    } else {
        ctx->tx_underruns++;
    }
    return ret;
}

static esp_err_t fd16_set_rx_cb(void *phy_ctx, uint8_t port,
                                void (*cb)(uint8_t, const uint8_t *, uint16_t, void *),
                                void *arg) {
    fd16_phy_ctx_t *ctx = (fd16_phy_ctx_t *)phy_ctx;
    ctx->rx_frame_cb = cb;
    ctx->rx_frame_cb_arg = arg;
    return ESP_OK;
}

static bool fd16_link_status(void *phy_ctx, uint8_t port) {
    /* FD16 has no CDR LOCK pin. Link is considered up when frames are received. */
    fd16_phy_ctx_t *ctx = (fd16_phy_ctx_t *)phy_ctx;
    return ctx->frames_rx > 0;
}

/* FD16 is full-duplex: no direction switching */
static esp_err_t fd16_set_direction(void *phy_ctx, uint8_t port, bool tx) {
    (void)phy_ctx; (void)port; (void)tx;
    return ESP_OK;
}

/* FD16 has no CDR recovered clock */
static uint32_t fd16_get_recovered_clock(void *phy_ctx, uint8_t port) {
    (void)phy_ctx; (void)port;
    return ABUS_FD16_CLOCK_HZ;
}

/* ---------------------------------------------------------------------------
 * PHY ops vtable
 * --------------------------------------------------------------------------- */

static const abus_phy_ops_t fd16_phy_ops = {
    .init               = fd16_init,
    .start              = fd16_start,
    .stop               = fd16_stop,
    .deinit             = fd16_deinit,
    .tx_frame           = fd16_tx_frame,
    .set_rx_callback    = fd16_set_rx_cb,
    .link_status        = fd16_link_status,
    .set_direction      = fd16_set_direction,
    .get_recovered_clock = fd16_get_recovered_clock,
};

/* ---------------------------------------------------------------------------
 * Factory function
 * --------------------------------------------------------------------------- */

esp_err_t abus_phy_fd16_create(const void *pin_config, abus_role_t role,
                               abus_phy_t *out_phy) {
    /* pin_config points to the fd16_pins struct in abus_pin_config_t */
    typedef struct {
        int8_t tx_data;
        int8_t rx_data;
        int8_t tx_clk;
        int8_t clk_in;
        int8_t clk_out;
        int8_t i2c_sda;
        int8_t i2c_scl;
    } fd16_pins_t;

    const fd16_pins_t *pins = (const fd16_pins_t *)pin_config;
    ESP_RETURN_ON_FALSE(pins && out_phy, ESP_ERR_INVALID_ARG, TAG, "null arg");

    fd16_phy_ctx_t *ctx = calloc(1, sizeof(fd16_phy_ctx_t));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "alloc ctx");

    ctx->role = role;
    ctx->tx_data_pin = pins->tx_data;
    ctx->rx_data_pin = pins->rx_data;
    ctx->tx_clk_pin  = pins->tx_clk;
    ctx->clk_in_pin  = pins->clk_in;
    ctx->clk_out_pin = pins->clk_out;
    ctx->i2c_sda_pin = pins->i2c_sda;
    ctx->i2c_scl_pin = pins->i2c_scl;

    out_phy->ops = &fd16_phy_ops;
    out_phy->ctx = ctx;

    ESP_LOGI(TAG, "FD16 PHY created: role=%s, tx=%d rx=%d clk_in=%d",
             role == ABUS_ROLE_MASTER ? "master" : "slave",
             ctx->tx_data_pin, ctx->rx_data_pin, ctx->clk_in_pin);
    return ESP_OK;
}
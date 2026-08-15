/*
 * AudioBus — FD16 fixed point-to-point full-duplex profile API
 *
 * A dedicated, discovery-free execution path for the FD16 product profile:
 *   16 audio channels in each direction, 32-bit signed samples, 48 kHz,
 *   72-byte fixed frame per sample, 27.648 Mb/s NRZ transport.
 *
 * Unlike the legacy AudioBus core (which performs multi-node discovery and
 * slot-map negotiation), FD16 is a fixed point-to-point link. The endpoint
 * knows at compile time that it operates with 16 channels / 48 kHz / 32-bit.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "audiobus_types.h"
#include "fd16_frame.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * --------------------------------------------------------------------------- */

typedef struct {
    abus_role_t role;               /* ABUS_ROLE_MASTER = clock master (A),
                                       ABUS_ROLE_SLAVE = clock receiver (B) */

    /* SN65LVDS049 pin assignments (see audiobus.h fd16_pins for semantics) */
    struct {
        int8_t tx_data;             /* PARLIO TX data[0] → SN65LVDS049 DIN1 */
        int8_t rx_data;             /* SN65LVDS049 ROUT1 → PARLIO RX data[0] */
        int8_t tx_clk;              /* PARLIO TX clock output (optional, -1) */
        int8_t clk_in;              /* 27.648 MHz external clock (TX + RX) */
        int8_t clk_out;             /* SN65LVDS049 DIN2 (clock driver, master only) */
        int8_t i2c_sda;             /* Si5351A I2C (master only, -1 if unused) */
        int8_t i2c_scl;
    } pins;

    /* Audio ring buffer depth in sample frames (default 8) */
    uint16_t audio_buffer_frames;

    /* Consecutive bad frames before output is muted (0 = never mute) */
    uint16_t concealment_threshold;
} abus_fd16_config_t;

/* Opaque handle */
typedef struct abus_fd16_handle *abus_fd16_handle_t;

/* ---------------------------------------------------------------------------
 * Statistics
 * --------------------------------------------------------------------------- */

typedef struct {
    uint32_t rx_frames;             /* Valid frames received */
    uint32_t tx_frames;             /* Frames transmitted */
    uint32_t crc_errors;            /* CRC-16 failures */
    uint32_t sync_errors;           /* SYNC pattern failures */
    uint32_t sequence_errors;       /* Unexpected sequence gaps */
    uint32_t rx_overruns;           /* RX buffer overruns */
    uint32_t tx_underruns;          /* TX underruns */
    uint32_t consecutive_errors;    /* Current consecutive bad-frame count */
    uint16_t last_seq;              /* Last valid received sequence */
} abus_fd16_stats_t;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------- */

/**
 * Initialize the FD16 link. Allocates DMA buffers and configures the PHY,
 * but does not start the link.
 */
esp_err_t abus_fd16_init(const abus_fd16_config_t *config, abus_fd16_handle_t *out_handle);

/**
 * Start the FD16 link. Both TX and RX begin operating simultaneously.
 */
esp_err_t abus_fd16_start(abus_fd16_handle_t handle);

/**
 * Stop the FD16 link. Audio output mutes.
 */
esp_err_t abus_fd16_stop(abus_fd16_handle_t handle);

/**
 * Release all resources.
 */
esp_err_t abus_fd16_deinit(abus_fd16_handle_t handle);

/* ---------------------------------------------------------------------------
 * Audio I/O — 16 channels of 32-bit samples
 * --------------------------------------------------------------------------- */

/**
 * Write 16 interleaved int32_t samples for transmission.
 * @return 1 on success, 0 if the TX ring buffer is full.
 */
int abus_fd16_audio_write(abus_fd16_handle_t handle, const int32_t samples[ABUS_FD16_CHANNELS]);

/**
 * Read 16 interleaved int32_t samples received from the remote endpoint.
 * @return 1 on success, 0 if no frame is available.
 */
int abus_fd16_audio_read(abus_fd16_handle_t handle, int32_t samples[ABUS_FD16_CHANNELS]);

/**
 * Get the number of received frames currently buffered.
 */
int abus_fd16_audio_available(abus_fd16_handle_t handle);

/* ---------------------------------------------------------------------------
 * Status
 * --------------------------------------------------------------------------- */

/**
 * Get FD16 link statistics.
 */
esp_err_t abus_fd16_get_stats(abus_fd16_handle_t handle, abus_fd16_stats_t *out_stats);

/**
 * Check if the link is up (valid frames received recently).
 */
bool abus_fd16_link_up(abus_fd16_handle_t handle);

#ifdef __cplusplus
}
#endif
/*
 * AudioBus — FD16 fixed point-to-point full-duplex frame format
 *
 * 16 audio channels in each direction, 32-bit signed samples, 48 kHz.
 * Fixed 72-byte frame per sample period, 27.648 Mb/s NRZ transport.
 * No 8b10b, no CDR, no turnaround, no direction switching.
 *
 * Frame layout (72 bytes = 576 bits):
 *   [0]     SYNC0      0xA5
 *   [1]     SYNC1      0x5A
 *   [2..3]  SEQUENCE   uint16_t, big-endian, increments every frame
 *   [4]     FLAGS      0x22 (48 kHz / 32-bit / FD16)
 *   [5]     CHANNELS   16
 *   [6..69] AUDIO      16 x int32_t, big-endian (MSB first)
 *   [70..71] CRC16      CRC-16-CCITT over bytes 0..69
 *
 * Invariant: 27_648_000 / 576 = 48_000 Hz exactly.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ABUS_FD16_CHANNELS       16
#define ABUS_FD16_AUDIO_BYTES    (ABUS_FD16_CHANNELS * sizeof(int32_t))
#define ABUS_FD16_HEADER_BYTES   6
#define ABUS_FD16_CRC_BYTES      2
#define ABUS_FD16_FRAME_BYTES    72
#define ABUS_FD16_FRAME_BITS     (ABUS_FD16_FRAME_BYTES * 8)
#define ABUS_FD16_BITRATE        27648000UL
#define ABUS_FD16_CLOCK_HZ       27648000UL
#define ABUS_FD16_SAMPLE_RATE    48000UL

/* Frame field offsets */
#define ABUS_FD16_SYNC0_OFFSET   0
#define ABUS_FD16_SYNC1_OFFSET   1
#define ABUS_FD16_SEQ_OFFSET     2
#define ABUS_FD16_FLAGS_OFFSET   4
#define ABUS_FD16_CH_OFFSET      5
#define ABUS_FD16_AUDIO_OFFSET   6
#define ABUS_FD16_CRC_OFFSET     (ABUS_FD16_FRAME_BYTES - ABUS_FD16_CRC_BYTES)

/* Sync bytes */
#define ABUS_FD16_SYNC0          0xA5
#define ABUS_FD16_SYNC1          0x5A

/* Flags: 48 kHz / 32-bit / FD16 */
#define ABUS_FD16_FLAGS          0x22

/* CRC-16-CCITT polynomial 0x1021, initial value 0xFFFF */
#define ABUS_FD16_CRC_POLY       0x1021
#define ABUS_FD16_CRC_INIT       0xFFFF

_Static_assert(ABUS_FD16_FRAME_BYTES == 72, "FD16 frame must be 72 bytes");
_Static_assert(ABUS_FD16_FRAME_BITS == 576, "FD16 frame must be 576 bits");
_Static_assert(ABUS_FD16_CLOCK_HZ / ABUS_FD16_FRAME_BITS == ABUS_FD16_SAMPLE_RATE,
               "27.648 MHz / 576 bits must equal 48 kHz");

/**
 * Compute CRC-16-CCITT over the given data.
 * Polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR.
 */
uint16_t abus_fd16_crc16(const uint8_t *data, size_t len);

/**
 * Pack 16 int32_t samples into a 72-byte FD16 frame.
 * Audio is serialized big-endian (MSB first), independent of CPU endianness.
 *
 * @param frame     Output buffer, must be at least ABUS_FD16_FRAME_BYTES long.
 * @param sequence  16-bit sequence number (increments every frame, wraps).
 * @param samples   16 interleaved int32_t samples.
 */
void abus_fd16_pack(uint8_t *frame, uint16_t sequence,
                    const int32_t samples[ABUS_FD16_CHANNELS]);

/**
 * Unpack and validate a 72-byte FD16 frame.
 *
 * @param frame     Input frame buffer (ABUS_FD16_FRAME_BYTES long).
 * @param sequence  Output: received sequence number (valid on success).
 * @param samples   Output: 16 int32_t samples (valid on success).
 * @return true if SYNC + CRC are valid, false otherwise.
 */
bool abus_fd16_unpack(const uint8_t *frame, uint16_t *sequence,
                      int32_t samples[ABUS_FD16_CHANNELS]);

/**
 * Validate SYNC pattern only (bytes 0..1).
 * @return true if SYNC0/SYNC1 match.
 */
bool abus_fd16_check_sync(const uint8_t *frame);

#ifdef __cplusplus
}
#endif
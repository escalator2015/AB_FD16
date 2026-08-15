/*
 * AudioBus — FD16 frame packing/unpacking and CRC-16-CCITT
 *
 * SPDX-License-Identifier: MIT
 */

#include "fd16_frame.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * CRC-16-CCITT (polynomial 0x1021, initial 0xFFFF, no reflection, no final XOR)
 * --------------------------------------------------------------------------- */

uint16_t abus_fd16_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = ABUS_FD16_CRC_INIT;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ ABUS_FD16_CRC_POLY;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ---------------------------------------------------------------------------
 * Big-endian serialization helpers (CPU-endianness independent)
 * --------------------------------------------------------------------------- */

static inline void put_u16_be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static inline uint16_t get_u16_be(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline void put_i32_be(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)(u >> 24);
    p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);
    p[3] = (uint8_t)(u & 0xFF);
}

static inline int32_t get_i32_be(const uint8_t *p) {
    uint32_t u = ((uint32_t)p[0] << 24) |
                 ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) |
                 ((uint32_t)p[3]);
    return (int32_t)u;
}

/* ---------------------------------------------------------------------------
 * Pack / unpack
 * --------------------------------------------------------------------------- */

void abus_fd16_pack(uint8_t *frame, uint16_t sequence,
                    const int32_t samples[ABUS_FD16_CHANNELS]) {
    /* Sync */
    frame[ABUS_FD16_SYNC0_OFFSET] = ABUS_FD16_SYNC0;
    frame[ABUS_FD16_SYNC1_OFFSET] = ABUS_FD16_SYNC1;

    /* Sequence (big-endian) */
    put_u16_be(&frame[ABUS_FD16_SEQ_OFFSET], sequence);

    /* Flags + channel count */
    frame[ABUS_FD16_FLAGS_OFFSET] = ABUS_FD16_FLAGS;
    frame[ABUS_FD16_CH_OFFSET] = ABUS_FD16_CHANNELS;

    /* Audio payload (big-endian int32) */
    for (int ch = 0; ch < ABUS_FD16_CHANNELS; ch++) {
        put_i32_be(&frame[ABUS_FD16_AUDIO_OFFSET + ch * sizeof(int32_t)],
                   samples[ch]);
    }

    /* CRC-16-CCITT over bytes 0..69 */
    uint16_t crc = abus_fd16_crc16(frame, ABUS_FD16_CRC_OFFSET);
    put_u16_be(&frame[ABUS_FD16_CRC_OFFSET], crc);
}

bool abus_fd16_check_sync(const uint8_t *frame) {
    if (!frame) return false;
    return frame[ABUS_FD16_SYNC0_OFFSET] == ABUS_FD16_SYNC0 &&
           frame[ABUS_FD16_SYNC1_OFFSET] == ABUS_FD16_SYNC1;
}

bool abus_fd16_unpack(const uint8_t *frame, uint16_t *sequence,
                      int32_t samples[ABUS_FD16_CHANNELS]) {
    if (!frame) return false;

    /* Validate SYNC */
    if (!abus_fd16_check_sync(frame)) {
        return false;
    }

    /* Validate CRC-16-CCITT over bytes 0..69 */
    uint16_t expected = get_u16_be(&frame[ABUS_FD16_CRC_OFFSET]);
    uint16_t computed = abus_fd16_crc16(frame, ABUS_FD16_CRC_OFFSET);
    if (expected != computed) {
        return false;
    }

    /* Extract sequence */
    if (sequence) {
        *sequence = get_u16_be(&frame[ABUS_FD16_SEQ_OFFSET]);
    }

    /* Extract audio samples (big-endian int32) */
    if (samples) {
        for (int ch = 0; ch < ABUS_FD16_CHANNELS; ch++) {
            samples[ch] = get_i32_be(&frame[ABUS_FD16_AUDIO_OFFSET + ch * sizeof(int32_t)]);
        }
    }

    return true;
}
/*
 * AudioBus — FD16 frame module unit tests (host-based, no ESP-IDF)
 *
 * Build: gcc -std=c11 -Wall -Wextra -I../../components/audiobus/include \
 *        test_fd16_frame.c ../../components/audiobus/src/fd16_frame.c -o test_fd16
 * Run:   ./test_fd16
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "fd16_frame.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        failures++; \
    } else { \
        printf("ok:   %s\n", msg); \
    } \
} while (0)

/* ---------------------------------------------------------------------------
 * Test 1: CRC-16-CCITT known vector ("123456789" -> 0x29B1)
 * --------------------------------------------------------------------------- */
static void test_crc_known_vector(void) {
    const uint8_t data[] = "123456789";
    uint16_t crc = abus_fd16_crc16(data, 9);
    CHECK(crc == 0x29B1, "CRC-16-CCITT known vector (0x29B1)");
}

/* ---------------------------------------------------------------------------
 * Test 2: frame size invariant
 * --------------------------------------------------------------------------- */
static void test_frame_size(void) {
    CHECK(ABUS_FD16_FRAME_BYTES == 72, "frame is 72 bytes");
    CHECK(ABUS_FD16_FRAME_BITS == 576, "frame is 576 bits");
    CHECK(ABUS_FD16_CLOCK_HZ / ABUS_FD16_FRAME_BITS == ABUS_FD16_SAMPLE_RATE,
          "27.648 MHz / 576 = 48 kHz");
}

/* ---------------------------------------------------------------------------
 * Test 3: round-trip pack/unpack for edge values
 * --------------------------------------------------------------------------- */
static void test_roundtrip_edge(void) {
    int32_t samples[ABUS_FD16_CHANNELS];
    uint8_t frame[ABUS_FD16_FRAME_BYTES];
    int32_t out[ABUS_FD16_CHANNELS];
    uint16_t seq = 0;

    /* INT32_MIN, INT32_MAX, zero, alternating */
    for (int ch = 0; ch < ABUS_FD16_CHANNELS; ch++) {
        switch (ch % 4) {
            case 0: samples[ch] = INT32_MIN; break;
            case 1: samples[ch] = INT32_MAX; break;
            case 2: samples[ch] = 0; break;
            case 3: samples[ch] = (ch & 1) ? 0x55555555 : (int32_t)0xAAAAAAAA; break;
        }
    }

    abus_fd16_pack(frame, 0x1234, samples);
    bool ok = abus_fd16_unpack(frame, &seq, out);
    CHECK(ok, "unpack returns true for valid frame");
    CHECK(seq == 0x1234, "sequence round-trips");
    CHECK(memcmp(samples, out, sizeof(samples)) == 0, "edge samples round-trip");
}

/* ---------------------------------------------------------------------------
 * Test 4: random round-trip
 * --------------------------------------------------------------------------- */
static void test_roundtrip_random(void) {
    srand(42);
    for (int iter = 0; iter < 1000; iter++) {
        int32_t samples[ABUS_FD16_CHANNELS];
        int32_t out[ABUS_FD16_CHANNELS];
        uint8_t frame[ABUS_FD16_FRAME_BYTES];
        uint16_t seq = (uint16_t)(rand() & 0xFFFF);

        for (int ch = 0; ch < ABUS_FD16_CHANNELS; ch++) {
            samples[ch] = (int32_t)rand();
        }

        abus_fd16_pack(frame, seq, samples);
        bool ok = abus_fd16_unpack(frame, NULL, out);
        if (!ok || memcmp(samples, out, sizeof(samples)) != 0) {
            CHECK(0, "random round-trip");
            return;
        }
    }
    CHECK(1, "1000 random round-trips");
}

/* ---------------------------------------------------------------------------
 * Test 5: sequence wrap 0xFFFF -> 0x0000
 * --------------------------------------------------------------------------- */
static void test_sequence_wrap(void) {
    int32_t samples[ABUS_FD16_CHANNELS];
    int32_t out[ABUS_FD16_CHANNELS];
    uint8_t frame[ABUS_FD16_FRAME_BYTES];
    uint16_t seq;

    memset(samples, 0, sizeof(samples));

    abus_fd16_pack(frame, 0xFFFF, samples);
    CHECK(abus_fd16_unpack(frame, &seq, out) && seq == 0xFFFF, "seq 0xFFFF");

    abus_fd16_pack(frame, 0x0000, samples);
    CHECK(abus_fd16_unpack(frame, &seq, out) && seq == 0x0000, "seq wraps to 0x0000");
}

/* ---------------------------------------------------------------------------
 * Test 6: corruption detection (one-bit and multi-bit)
 * --------------------------------------------------------------------------- */
static void test_corruption(void) {
    int32_t samples[ABUS_FD16_CHANNELS];
    int32_t out[ABUS_FD16_CHANNELS];
    uint8_t frame[ABUS_FD16_FRAME_BYTES];

    memset(samples, 0, sizeof(samples));
    abus_fd16_pack(frame, 0x42, samples);

    /* One-bit corruption in audio payload */
    uint8_t f1[ABUS_FD16_FRAME_BYTES];
    memcpy(f1, frame, sizeof(f1));
    f1[ABUS_FD16_AUDIO_OFFSET + 5] ^= 0x01;
    CHECK(!abus_fd16_unpack(f1, NULL, out), "one-bit corruption rejected");

    /* Multi-bit corruption in header */
    uint8_t f2[ABUS_FD16_FRAME_BYTES];
    memcpy(f2, frame, sizeof(f2));
    f2[ABUS_FD16_SEQ_OFFSET] ^= 0xFF;
    f2[ABUS_FD16_SEQ_OFFSET + 1] ^= 0xFF;
    CHECK(!abus_fd16_unpack(f2, NULL, out), "multi-bit corruption rejected");

    /* Corrupted SYNC */
    uint8_t f3[ABUS_FD16_FRAME_BYTES];
    memcpy(f3, frame, sizeof(f3));
    f3[ABUS_FD16_SYNC0_OFFSET] = 0x00;
    CHECK(!abus_fd16_unpack(f3, NULL, out), "bad SYNC0 rejected");

    /* Corrupted CRC field itself */
    uint8_t f4[ABUS_FD16_FRAME_BYTES];
    memcpy(f4, frame, sizeof(f4));
    f4[ABUS_FD16_CRC_OFFSET] ^= 0x80;
    CHECK(!abus_fd16_unpack(f4, NULL, out), "corrupted CRC rejected");
}

/* ---------------------------------------------------------------------------
 * Test 7: sync check helper
 * --------------------------------------------------------------------------- */
static void test_sync_check(void) {
    uint8_t frame[ABUS_FD16_FRAME_BYTES];
    memset(frame, 0, sizeof(frame));
    frame[0] = ABUS_FD16_SYNC0;
    frame[1] = ABUS_FD16_SYNC1;
    CHECK(abus_fd16_check_sync(frame), "sync check passes on valid sync");
    frame[1] = 0x00;
    CHECK(!abus_fd16_check_sync(frame), "sync check fails on bad sync");
}

/* ---------------------------------------------------------------------------
 * Test 8: wire byte order is big-endian (deterministic)
 * --------------------------------------------------------------------------- */
static void test_wire_byte_order(void) {
    int32_t samples[ABUS_FD16_CHANNELS];
    uint8_t frame[ABUS_FD16_FRAME_BYTES];

    memset(samples, 0, sizeof(samples));
    samples[0] = 0x01020304; /* Positive value with distinct bytes */

    abus_fd16_pack(frame, 0, samples);

    /* Audio[0] at offset 6 should be 01 02 03 04 (MSB first) */
    CHECK(frame[ABUS_FD16_AUDIO_OFFSET + 0] == 0x01, "sample[0] byte0 = 0x01");
    CHECK(frame[ABUS_FD16_AUDIO_OFFSET + 1] == 0x02, "sample[0] byte1 = 0x02");
    CHECK(frame[ABUS_FD16_AUDIO_OFFSET + 2] == 0x03, "sample[0] byte2 = 0x03");
    CHECK(frame[ABUS_FD16_AUDIO_OFFSET + 3] == 0x04, "sample[0] byte3 = 0x04");

    /* Sequence 0x1234 at offset 2 should be 12 34 */
    abus_fd16_pack(frame, 0x1234, samples);
    CHECK(frame[ABUS_FD16_SEQ_OFFSET] == 0x12, "seq byte0 = 0x12");
    CHECK(frame[ABUS_FD16_SEQ_OFFSET + 1] == 0x34, "seq byte1 = 0x34");
}

int main(void) {
    printf("=== AudioBus FD16 frame unit tests ===\n");
    test_crc_known_vector();
    test_frame_size();
    test_roundtrip_edge();
    test_roundtrip_random();
    test_sequence_wrap();
    test_corruption();
    test_sync_check();
    test_wire_byte_order();

    printf("\n%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
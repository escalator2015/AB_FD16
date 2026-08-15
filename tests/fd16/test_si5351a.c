/*
 * AudioBus — Si5351A parameter computation unit tests (host-based)
 *
 * Build: gcc -std=c11 -Wall -Wextra -I../../components/audiobus/include \
 *        -Istubs -Istubs/driver \
 *        test_si5351a.c ../../components/audiobus/src/si5351a.c -o test_si5351a
 * Run:   ./test_si5351a
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "si5351a.h"

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
 * Test 1: 27.648 MHz with 25 MHz crystal
 * Algorithm picks smallest N in [600,900] MHz range:
 *   n_min = ceil(600M / 27.648M) = 22
 *   vco = 27.648 × 22 = 608.256 MHz
 *   a = 608.256M / 25M = 24, rem = 8.256M
 *   b/c = 8.256M / 25M = 8256/25000 → gcd=8 → b=1032, c=3125
 * --------------------------------------------------------------------------- */
static void test_27648_25mhz(void) {
    abus_si5351a_params_t p;
    esp_err_t ret = abus_si5351a_compute(25000000, 27648000, &p);
    CHECK(ret == ESP_OK, "compute 27.648 MHz / 25 MHz returns OK");

    /* fVCO = 27.648 × 22 = 608.256 MHz */
    CHECK(p.vco_hz == 608256000, "VCO = 608.256 MHz");
    CHECK(p.ms_div == 22, "output divider = 22");

    /* PLL: a + b/c = 608.256 / 25 = 24.33024 → a=24, b=1032, c=3125 */
    CHECK(p.pll_a == 24, "PLL integer a = 24");
    CHECK(p.pll_b == 1032, "PLL numerator b = 1032");
    CHECK(p.pll_c == 3125, "PLL denominator c = 3125");

    /* Verify: fVCO = xtal × (a + b/c) */
    uint64_t vco_check = (uint64_t)25000000 * p.pll_a +
                         (uint64_t)25000000 * p.pll_b / p.pll_c;
    CHECK(vco_check == p.vco_hz, "VCO recomputed from params matches");
}

/* ---------------------------------------------------------------------------
 * Test 2: 27.648 MHz with 27 MHz crystal
 *   n_min = ceil(600M / 27.648M) = 22
 *   vco = 27.648 × 22 = 608.256 MHz
 *   a = 608.256M / 27M = 22, rem = 14.256M
 *   b/c = 14.256M / 27M = 14256000/27000000 → gcd=216000 → b=66, c=125
 * --------------------------------------------------------------------------- */
static void test_27648_27mhz(void) {
    abus_si5351a_params_t p;
    esp_err_t ret = abus_si5351a_compute(27000000, 27648000, &p);
    CHECK(ret == ESP_OK, "compute 27.648 MHz / 27 MHz returns OK");

    /* fVCO = 27.648 × 22 = 608.256 MHz */
    CHECK(p.vco_hz == 608256000, "VCO = 608.256 MHz");
    CHECK(p.ms_div == 22, "output divider = 22");

    /* PLL: a + b/c = 608.256 / 27 = 22.528 → a=22, b=66, c=125 */
    CHECK(p.pll_a == 22, "PLL integer a = 22");
    CHECK(p.pll_b == 66, "PLL numerator b = 66");
    CHECK(p.pll_c == 125, "PLL denominator c = 125");

    uint64_t vco_check = (uint64_t)27000000 * p.pll_a +
                         (uint64_t)27000000 * p.pll_b / p.pll_c;
    CHECK(vco_check == p.vco_hz, "VCO recomputed from params matches");
}

/* ---------------------------------------------------------------------------
 * Test 3: VCO stays in [600, 900] MHz for various frequencies
 * --------------------------------------------------------------------------- */
static void test_vco_range(void) {
    const uint32_t freqs[] = { 1000000, 10000000, 27648000, 50000000, 100000000 };
    for (size_t i = 0; i < sizeof(freqs)/sizeof(freqs[0]); i++) {
        abus_si5351a_params_t p;
        esp_err_t ret = abus_si5351a_compute(25000000, freqs[i], &p);
        if (ret != ESP_OK) {
            printf("FAIL: freq %lu rejected (line %d)\n",
                   (unsigned long)freqs[i], __LINE__);
            failures++;
            continue;
        }
        if (p.vco_hz < ABUS_SI5351A_VCO_MIN_HZ ||
            p.vco_hz > ABUS_SI5351A_VCO_MAX_HZ) {
            printf("FAIL: VCO %lu out of range for %lu (line %d)\n",
                   (unsigned long)p.vco_hz, (unsigned long)freqs[i], __LINE__);
            failures++;
        }
    }
    printf("ok:   VCO in [600,900] MHz for 5 test frequencies\n");
}

/* ---------------------------------------------------------------------------
 * Test 4: invalid arguments
 * --------------------------------------------------------------------------- */
static void test_invalid(void) {
    abus_si5351a_params_t p;
    CHECK(abus_si5351a_compute(0, 27648000, &p) == ESP_ERR_INVALID_ARG,
          "zero xtal rejected");
    CHECK(abus_si5351a_compute(25000000, 0, &p) == ESP_ERR_INVALID_ARG,
          "zero clk rejected");
    CHECK(abus_si5351a_compute(25000000, 27648000, NULL) == ESP_ERR_INVALID_ARG,
          "null out rejected");

    /* 1 Hz would need VCO = 600 MHz → div = 600M, out of multisynth range */
    CHECK(abus_si5351a_compute(25000000, 1, &p) == ESP_ERR_INVALID_ARG,
          "1 Hz rejected (div out of range)");
}

/* ---------------------------------------------------------------------------
 * Test 5: output divider within multisynth range
 * --------------------------------------------------------------------------- */
static void test_div_range(void) {
    abus_si5351a_params_t p;
    /* 100 MHz: VCO = 100 × 6 = 600 MHz, div = 6 (>= 4) */
    CHECK(abus_si5351a_compute(25000000, 100000000, &p) == ESP_OK,
          "100 MHz OK");
    CHECK(p.ms_div >= ABUS_SI5351A_MS_DIV_MIN, "div >= 4");
    CHECK(p.ms_div <= ABUS_SI5351A_MS_DIV_MAX, "div <= 900");
}

int main(void) {
    printf("=== AudioBus Si5351A parameter tests ===\n");
    test_27648_25mhz();
    test_27648_27mhz();
    test_vco_range();
    test_invalid();
    test_div_range();

    printf("\n%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
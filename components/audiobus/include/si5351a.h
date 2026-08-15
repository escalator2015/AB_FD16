/*
 * AudioBus — Si5351A I2C clock generator driver
 *
 * Programs the Si5351A (Silicon Labs) via I2C to generate a desired
 * frequency on CLK0. Used on the FD16 master node (A) to produce the
 * 27.648 MHz transport clock.
 *
 * The Si5351A uses a fractional PLL (multisynth) + output divider:
 *   fVCO = fXTAL × (a + b/c)          // PLL multisynth (a=6..180, b<c, c=1..1048575)
 *   fCLK = fVCO / (div + r/2)         // output multisynth (div=4..900, r=0/1)
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ABUS_SI5351A_I2C_ADDR   0x60    /* 7-bit I2C address */

/* Si5351A register map (relevant subset) */
#define ABUS_SI5351A_REG_CLK0_CTRL       16
#define ABUS_SI5351A_REG_CLK1_CTRL       17
#define ABUS_SI5351A_REG_CLK2_CTRL       18
#define ABUS_SI5351A_REG_CLK3_CTRL       19
#define ABUS_SI5351A_REG_PLL_RESET       177
#define ABUS_SI5351A_REG_CLK0_DIS_STATE  183
#define ABUS_SI5351A_REG_CLK0_PD         187

/* PLL A multisynth registers (16..23) */
#define ABUS_SI5351A_REG_MSNA_BASE       26
#define ABUS_SI5351A_REG_MSNA_LEN        8

/* CLK0 output multisynth registers (42..49) */
#define ABUS_SI5351A_REG_MS0_BASE        42
#define ABUS_SI5351A_REG_MS0_LEN         8

/* CLK0 control: enable, source = PLL A */
#define ABUS_SI5351A_CLK0_CTRL_ENABLE    0x4F
#define ABUS_SI5351A_CLK0_CTRL_DISABLE   0x80

/* PLL A source select (register 3) */
#define ABUS_SI5351A_REG_CLK_SRC         3
#define ABUS_SI5351A_CLK0_SRC_PLLA       0x00

/* PLL reset bit (register 177) */
#define ABUS_SI5351A_PLL_RESET_BIT       0x80

/* CLK0 power-down (register 187) */
#define ABUS_SI5351A_CLK0_PD_ON          0x80
#define ABUS_SI5351A_CLK0_PD_OFF         0x00

/* CLK0 disable state (register 183) */
#define ABUS_SI5351A_CLK0_DIS_LOW        0x00

/* PLL valid range */
#define ABUS_SI5351A_VCO_MIN_HZ          600000000UL
#define ABUS_SI5351A_VCO_MAX_HZ          900000000UL

/* Output multisynth divider range */
#define ABUS_SI5351A_MS_DIV_MIN          4
#define ABUS_SI5351A_MS_DIV_MAX          900

/* PLL multisynth integer range */
#define ABUS_SI5351A_PLL_A_MIN           6
#define ABUS_SI5351A_PLL_A_MAX           180

/* Computed multisynth parameters for a single output */
typedef struct {
    uint32_t pll_a;         /* PLL integer divider */
    uint32_t pll_b;         /* PLL fractional numerator */
    uint32_t pll_c;         /* PLL fractional denominator */
    uint32_t ms_div;        /* Output multisynth integer divider */
    uint32_t ms_r;          /* Output multisynth fractional (0 or 1) */
    uint32_t vco_hz;        /* Resulting VCO frequency */
} abus_si5351a_params_t;

typedef struct {
    int8_t   sda_pin;       /* I2C SDA GPIO */
    int8_t   scl_pin;       /* I2C SCL GPIO */
    uint32_t i2c_clk_hz;    /* I2C bus clock (e.g. 400000) */
    uint32_t xtal_hz;       /* Reference crystal frequency (e.g. 25000000) */
    uint32_t clk0_hz;       /* Desired CLK0 frequency (e.g. 27648000) */
} abus_si5351a_config_t;

/**
 * Compute the Si5351A multisynth parameters for a given crystal and
 * desired output frequency. Pure integer math (no float).
 *
 * @param xtal_hz  Reference crystal frequency.
 * @param clk_hz   Desired output frequency.
 * @param out      Output parameters.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range.
 */
esp_err_t abus_si5351a_compute(uint32_t xtal_hz, uint32_t clk_hz,
                               abus_si5351a_params_t *out);

/**
 * Initialize the I2C bus and program the Si5351A to output clk0_hz on CLK0.
 * Only call on the master node (A).
 */
esp_err_t abus_si5351a_init(const abus_si5351a_config_t *cfg);

/**
 * Re-program CLK0 to a new frequency (after init).
 */
esp_err_t abus_si5351a_set_clk0(uint32_t freq_hz);

/**
 * Release the I2C bus resources.
 */
esp_err_t abus_si5351a_deinit(void);

#ifdef __cplusplus
}
#endif
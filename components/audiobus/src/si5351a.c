/*
 * AudioBus — Si5351A I2C clock generator driver
 *
 * Programs the Si5351A (Silicon Labs) via I2C to generate a desired
 * frequency on CLK0. Used on the FD16 master node (A) to produce the
 * 27.648 MHz transport clock.
 *
 * SPDX-License-Identifier: MIT
 */

#include "si5351a.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"

#include <string.h>

static const char *TAG = "abus_si5351a";

/* ---------------------------------------------------------------------------
 * Internal I2C state
 * --------------------------------------------------------------------------- */

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static uint32_t s_xtal_hz = 0;
static uint32_t s_clk0_hz = 0;

/* ---------------------------------------------------------------------------
 * Register write helper
 * --------------------------------------------------------------------------- */

static esp_err_t si5351a_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t si5351a_write_regs(uint8_t reg, const uint8_t *vals, size_t len) {
    uint8_t buf[1 + 8];
    if (len > 8) return ESP_ERR_INVALID_SIZE;
    buf[0] = reg;
    memcpy(&buf[1], vals, len);
    return i2c_master_transmit(s_dev, buf, 1 + len, 100);
}

/* ---------------------------------------------------------------------------
 * Multisynth register encoding
 *
 * The Si5351A stores a/b/c in a 20-bit field:
 *   [19:0] = c (20 bits), then b (20 bits), then a (8 bits)
 * Each 8-bit register holds 8 bits of the 20-bit value, MSB first.
 * --------------------------------------------------------------------------- */

static void encode_ms_regs(uint32_t a, uint32_t b, uint32_t c,
                           uint8_t out[8]) {
    /* c: 20 bits, b: 20 bits, a: 8 bits */
    uint64_t packed = ((uint64_t)a << 40) | ((uint64_t)b << 20) | c;
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)((packed >> (56 - 8 * i)) & 0xFF);
    }
}

/* ---------------------------------------------------------------------------
 * Parameter computation (pure integer math)
 *
 * fVCO = fXTAL × (a + b/c)   in [600, 900] MHz
 * fCLK = fVCO / (div + r/2)
 *
 * Strategy: choose an integer output divider N such that
 *   fVCO = fCLK × N  is in [600, 900] MHz.
 * Then compute the PLL fractional part:
 *   a + b/c = fVCO / fXTAL
 * --------------------------------------------------------------------------- */

esp_err_t abus_si5351a_compute(uint32_t xtal_hz, uint32_t clk_hz,
                               abus_si5351a_params_t *out) {
    ESP_RETURN_ON_FALSE(xtal_hz > 0 && clk_hz > 0 && out,
                        ESP_ERR_INVALID_ARG, TAG, "bad arg");

    /* Find output divider N such that fVCO = clk_hz × N is in [600, 900] MHz */
    uint32_t n_min = (ABUS_SI5351A_VCO_MIN_HZ + clk_hz - 1) / clk_hz;
    uint32_t n_max = ABUS_SI5351A_VCO_MAX_HZ / clk_hz;

    if (n_min > n_max) {
        ESP_LOGE(TAG, "No valid output divider for %lu Hz", (unsigned long)clk_hz);
        return ESP_ERR_INVALID_ARG;
    }

    /* Clamp to multisynth divider range */
    if (n_min < ABUS_SI5351A_MS_DIV_MIN) n_min = ABUS_SI5351A_MS_DIV_MIN;
    if (n_max > ABUS_SI5351A_MS_DIV_MAX) n_max = ABUS_SI5351A_MS_DIV_MAX;
    if (n_min > n_max) {
        ESP_LOGE(TAG, "Output divider out of multisynth range");
        return ESP_ERR_INVALID_ARG;
    }

    /* Prefer the smallest N (largest VCO, best jitter) */
    uint32_t n = n_min;
    uint32_t vco_hz = clk_hz * n;

    /* PLL: a + b/c = vco_hz / xtal_hz */
    uint32_t a = vco_hz / xtal_hz;
    uint64_t rem = (uint64_t)vco_hz % xtal_hz;

    if (a < ABUS_SI5351A_PLL_A_MIN || a > ABUS_SI5351A_PLL_A_MAX) {
        ESP_LOGE(TAG, "PLL integer %lu out of range", (unsigned long)a);
        return ESP_ERR_INVALID_ARG;
    }

    /* Fractional part: b/c = rem / xtal_hz.
     * Use c = xtal_hz (up to 1048575) and b = rem, then reduce. */
    uint32_t c = xtal_hz;
    uint32_t b = (uint32_t)rem;

    /* Reduce b/c by gcd */
    uint32_t x = b, y = c;
    while (y) { uint32_t t = x % y; x = y; y = t; }
    uint32_t g = x;
    if (g > 1) { b /= g; c /= g; }

    /* c must be <= 1048575 */
    if (c > 1048575) {
        ESP_LOGE(TAG, "PLL denominator %lu too large", (unsigned long)c);
        return ESP_ERR_INVALID_ARG;
    }

    out->pll_a = a;
    out->pll_b = b;
    out->pll_c = c;
    out->ms_div = n;
    out->ms_r = 0;
    out->vco_hz = vco_hz;

    ESP_LOGD(TAG, "compute: xtal=%lu clk=%lu → a=%lu b=%lu c=%lu div=%lu vco=%lu",
             (unsigned long)xtal_hz, (unsigned long)clk_hz,
             (unsigned long)a, (unsigned long)b, (unsigned long)c,
             (unsigned long)n, (unsigned long)vco_hz);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Program the Si5351A for CLK0
 * --------------------------------------------------------------------------- */

static esp_err_t si5351a_program_clk0(uint32_t xtal_hz, uint32_t clk_hz) {
    abus_si5351a_params_t p;
    ESP_RETURN_ON_ERROR(abus_si5351a_compute(xtal_hz, clk_hz, &p),
                        TAG, "compute failed");

    /* 1. Power down CLK0 while configuring */
    ESP_RETURN_ON_ERROR(si5351a_write_reg(ABUS_SI5351A_REG_CLK0_PD,
                                          ABUS_SI5351A_CLK0_PD_ON), TAG, "pd on");

    /* 2. CLK0 source = PLL A */
    ESP_RETURN_ON_ERROR(si5351a_write_reg(ABUS_SI5351A_REG_CLK_SRC,
                                          ABUS_SI5351A_CLK0_SRC_PLLA), TAG, "src");

    /* 3. Program PLL A multisynth (registers 26..33) */
    uint8_t msna[ABUS_SI5351A_REG_MSNA_LEN];
    encode_ms_regs(p.pll_a, p.pll_b, p.pll_c, msna);
    ESP_RETURN_ON_ERROR(si5351a_write_regs(ABUS_SI5351A_REG_MSNA_BASE, msna,
                                           ABUS_SI5351A_REG_MSNA_LEN), TAG, "msna");

    /* 4. Program CLK0 output multisynth (registers 42..49).
     *    For integer divider (r=0), the fractional part is 0. */
    uint8_t ms0[ABUS_SI5351A_REG_MS0_LEN];
    encode_ms_regs(p.ms_div, 0, 1, ms0);
    ESP_RETURN_ON_ERROR(si5351a_write_regs(ABUS_SI5351A_REG_MS0_BASE, ms0,
                                           ABUS_SI5351A_REG_MS0_LEN), TAG, "ms0");

    /* 5. CLK0 control: enable, source = PLL A */
    ESP_RETURN_ON_ERROR(si5351a_write_reg(ABUS_SI5351A_REG_CLK0_CTRL,
                                          ABUS_SI5351A_CLK0_CTRL_ENABLE), TAG, "ctrl");

    /* 6. CLK0 disable state = low */
    ESP_RETURN_ON_ERROR(si5351a_write_reg(ABUS_SI5351A_REG_CLK0_DIS_STATE,
                                          ABUS_SI5351A_CLK0_DIS_LOW), TAG, "dis");

    /* 7. Power up CLK0 */
    ESP_RETURN_ON_ERROR(si5351a_write_reg(ABUS_SI5351A_REG_CLK0_PD,
                                          ABUS_SI5351A_CLK0_PD_OFF), TAG, "pd off");

    /* 8. Reset PLL A to apply the new configuration */
    ESP_RETURN_ON_ERROR(si5351a_write_reg(ABUS_SI5351A_REG_PLL_RESET,
                                          ABUS_SI5351A_PLL_RESET_BIT), TAG, "pll reset");

    ESP_LOGI(TAG, "CLK0 programmed: %lu Hz (a=%lu b=%lu c=%lu div=%lu vco=%lu)",
             (unsigned long)clk_hz,
             (unsigned long)p.pll_a, (unsigned long)p.pll_b, (unsigned long)p.pll_c,
             (unsigned long)p.ms_div, (unsigned long)p.vco_hz);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

esp_err_t abus_si5351a_init(const abus_si5351a_config_t *cfg) {
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "null cfg");
    ESP_RETURN_ON_FALSE(cfg->sda_pin >= 0 && cfg->scl_pin >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "bad I2C pins");
    ESP_RETURN_ON_FALSE(cfg->xtal_hz > 0 && cfg->clk0_hz > 0,
                        ESP_ERR_INVALID_ARG, TAG, "bad freq");

    if (s_bus) {
        ESP_LOGW(TAG, "Si5351A already initialized");
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = cfg->sda_pin,
        .scl_io_num = cfg->scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "bus create");

    /* --- Scan the I2C bus and log detected devices --- */
    ESP_LOGI(TAG, "Scanning I2C bus (SDA=%d SCL=%d)...", cfg->sda_pin, cfg->scl_pin);
    int found = 0;
    for (uint16_t addr = 0x03; addr <= 0x77; addr++) {
        esp_err_t probe = i2c_master_probe(s_bus, addr, 50);
        if (probe == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X%s", addr,
                     addr == ABUS_SI5351A_I2C_ADDR ? " (Si5351A)" : "");
            found++;
        }
    }
    ESP_LOGI(TAG, "I2C scan complete: %d device(s) found", found);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ABUS_SI5351A_I2C_ADDR,
        .scl_speed_hz = cfg->i2c_clk_hz ? cfg->i2c_clk_hz : 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev),
                        TAG, "dev add");

    s_xtal_hz = cfg->xtal_hz;
    s_clk0_hz = cfg->clk0_hz;

    ESP_RETURN_ON_ERROR(si5351a_program_clk0(s_xtal_hz, s_clk0_hz),
                        TAG, "program CLK0");

    ESP_LOGI(TAG, "Si5351A initialized: xtal=%lu CLK0=%lu",
             (unsigned long)s_xtal_hz, (unsigned long)s_clk0_hz);
    return ESP_OK;
}

esp_err_t abus_si5351a_set_clk0(uint32_t freq_hz) {
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(freq_hz > 0, ESP_ERR_INVALID_ARG, TAG, "bad freq");
    s_clk0_hz = freq_hz;
    return si5351a_program_clk0(s_xtal_hz, s_clk0_hz);
}

esp_err_t abus_si5351a_deinit(void) {
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    s_xtal_hz = 0;
    s_clk0_hz = 0;
    return ESP_OK;
}
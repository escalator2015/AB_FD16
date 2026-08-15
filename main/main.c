/*
 * AudioBus FD16 Node — ESP32-P4
 *
 * Full-duplex fixed point-to-point profile:
 *   16 audio channels in each direction, 32-bit samples, 48 kHz,
 *   72-byte fixed frame, 27.648 Mb/s NRZ transport over SN65LVDS049.
 *
 * GPIO pins are configured via menuconfig (idf.py menuconfig →
 * "AudioBus FD16 Configuration" → "FD16 GPIO Pin Configuration").
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "audiobus_fd16.h"

static const char *TAG = "fd16_example";

/* Read the role-select GPIO at startup:
 * HIGH (1) = master/clock source A, LOW (0) = slave/clock receiver B.
 * If the pin is not wired (-1), default to master (A). */
static abus_role_t detect_role(void) {
    int8_t pin = CONFIG_ABUS_FD16_PIN_ROLE;
    if (pin < 0) {
        ESP_LOGI(TAG, "Role-select GPIO not wired (-1), defaulting to MASTER (A)");
        return ABUS_ROLE_MASTER;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    int level = gpio_get_level(pin);
    ESP_LOGI(TAG, "Role-select GPIO %d = %d → %s", pin, level,
             level ? "MASTER (A)" : "SLAVE (B)");
    return level ? ABUS_ROLE_MASTER : ABUS_ROLE_SLAVE;
}

/* Generate a 16-channel sine-wave test tone */
static void audio_generator_task(void *arg) {
    abus_fd16_handle_t link = (abus_fd16_handle_t)arg;
    float phase = 0.0f;
    const float phase_inc = 2.0f * M_PI * 440.0f / 48000.0f;
    int32_t samples[ABUS_FD16_CHANNELS];

    while (1) {
        float val = 0.5f * sinf(phase);
        for (int ch = 0; ch < ABUS_FD16_CHANNELS; ch++) {
            samples[ch] = (int32_t)(val * 2147483647.0f);
        }
        phase += phase_inc;
        if (phase >= 2.0f * M_PI) phase -= 2.0f * M_PI;

        while (abus_fd16_audio_write(link, samples) == 0) {
            vTaskDelay(1);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "AudioBus FD16 Node — SN65LVDS049 full-duplex");

    abus_fd16_config_t config = {
        .role = detect_role(),
        .pins = {
            .tx_data = CONFIG_ABUS_FD16_PIN_TX_DATA,
            .rx_data = CONFIG_ABUS_FD16_PIN_RX_DATA,
            .tx_clk  = CONFIG_ABUS_FD16_PIN_TX_CLK,
            .clk_in  = CONFIG_ABUS_FD16_PIN_CLK_IN,
            .clk_out = CONFIG_ABUS_FD16_PIN_CLK_OUT,
            .i2c_sda = CONFIG_ABUS_FD16_PIN_I2C_SDA,
            .i2c_scl = CONFIG_ABUS_FD16_PIN_I2C_SCL,
        },
        .audio_buffer_frames = CONFIG_ABUS_FD16_AUDIO_BUF_FRAMES,
        .concealment_threshold = CONFIG_ABUS_FD16_CONCEALMENT_THRESHOLD,
    };

    abus_fd16_handle_t link;
    esp_err_t ret = abus_fd16_init(&config, &link);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "abus_fd16_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = abus_fd16_start(link);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "abus_fd16_start failed: %s", esp_err_to_name(ret));
        return;
    }

    xTaskCreate(audio_generator_task, "fd16_gen", 4096, link, 10, NULL);

    /* Status reporting */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        abus_fd16_stats_t stats;
        abus_fd16_get_stats(link, &stats);
        ESP_LOGI(TAG, "tx=%lu rx=%lu crc=%lu sync=%lu seq=%lu link=%d",
                 stats.tx_frames, stats.rx_frames,
                 stats.crc_errors, stats.sync_errors,
                 stats.sequence_errors, abus_fd16_link_up(link));
    }
}
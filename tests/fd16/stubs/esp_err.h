/* Host test stub for ESP-IDF esp_err.h */
#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK          0
#define ESP_FAIL        (-1)
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE  0x105
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERR_NOT_SUPPORTED 0x106

#define ESP_RETURN_ON_FALSE(cond, err, tag, fmt, ...) do { \
    if (!(cond)) { return (err); } \
} while (0)

#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...) do { \
    esp_err_t err_rc_ = (x); \
    if (err_rc_ != ESP_OK) { return err_rc_; } \
} while (0)

#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); } while (0)

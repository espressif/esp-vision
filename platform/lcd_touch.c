/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lcd_touch.h"

#include <stddef.h>

#include "sdkconfig.h"

#if defined(ESP_VISION_USE_BOARD_MANAGER) && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT)
#include "esp_board_manager.h"
#include "dev_lcd_touch.h"
#include "esp_log.h"

static const char *TAG = "esp_vision_lcd_touch";
static esp_lcd_touch_handle_t s_lcd_touch;
static bool s_initialized;

esp_err_t esp_vision_lcd_touch_init(void)
{
    if (s_lcd_touch != NULL) {
        return ESP_OK;
    }
    if (!esp_board_manager_check_name("lcd_touch")) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_initialized) {
        esp_vision_lcd_touch_deinit();
        if (s_initialized) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    esp_err_t ret = esp_board_manager_init_device_by_name("lcd_touch");
    if (ret != ESP_OK) {
        return ret;
    }
    s_initialized = true;

    dev_lcd_touch_handles_t *handles = NULL;
    ret = esp_board_manager_get_device_handle("lcd_touch", (void **)&handles);
    if ((ret != ESP_OK) || (handles == NULL) || (handles->touch_handle == NULL)) {
        esp_vision_lcd_touch_deinit();
        return (ret == ESP_OK) ? ESP_ERR_INVALID_STATE : ret;
    }
    s_lcd_touch = handles->touch_handle;
    return ESP_OK;
}

void esp_vision_lcd_touch_deinit(void)
{
    s_lcd_touch = NULL;
    if (s_initialized) {
        esp_err_t ret = esp_board_manager_deinit_device_by_name("lcd_touch");
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to deinitialize LCD touch: %s", esp_err_to_name(ret));
            return;
        }
        s_initialized = false;
    }
}

esp_err_t esp_vision_lcd_touch_read(esp_vision_lcd_touch_point_t points[ESP_VISION_LCD_TOUCH_MAX_POINTS], uint8_t *count)
{
    if ((points == NULL) || (count == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (s_lcd_touch == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_lcd_touch_read_data(s_lcd_touch);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_lcd_touch_point_data_t data[ESP_VISION_LCD_TOUCH_MAX_POINTS];
    ret = esp_lcd_touch_get_data(s_lcd_touch, data, count, ESP_VISION_LCD_TOUCH_MAX_POINTS);
    if (ret != ESP_OK) {
        *count = 0;
        return ret;
    }
    for (uint8_t i = 0; i < *count; ++i) {
        points[i].x = data[i].x;
        points[i].y = data[i].y;
    }
    return ESP_OK;
}

#else

esp_err_t esp_vision_lcd_touch_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void esp_vision_lcd_touch_deinit(void)
{
}

esp_err_t esp_vision_lcd_touch_read(esp_vision_lcd_touch_point_t points[ESP_VISION_LCD_TOUCH_MAX_POINTS], uint8_t *count)
{
    (void)points;
    if (count != NULL) {
        *count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

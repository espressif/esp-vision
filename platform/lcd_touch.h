/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#define ESP_VISION_LCD_TOUCH_MAX_POINTS (5)

typedef struct {
    uint16_t x;
    uint16_t y;
} esp_vision_lcd_touch_point_t;

esp_err_t esp_vision_lcd_touch_init(void);
void esp_vision_lcd_touch_deinit(void);
esp_err_t esp_vision_lcd_touch_read(esp_vision_lcd_touch_point_t points[ESP_VISION_LCD_TOUCH_MAX_POINTS], uint8_t *count);

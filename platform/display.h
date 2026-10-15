/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct image image_t;
typedef struct rectangle rectangle_t;

typedef struct {
    int x;
    int y;
    float x_scale;
    float y_scale;
    bool fit;
    const rectangle_t *roi;
} esp_vision_display_draw_config_t;

void esp_vision_display_init0(void);
uint32_t esp_vision_display_get_default_width(void);
uint32_t esp_vision_display_get_default_height(void);
esp_err_t esp_vision_display_init(uint32_t width, uint32_t height, uint32_t backlight);
void esp_vision_display_deinit(void);
bool esp_vision_display_is_ready(void);
uint32_t esp_vision_display_get_width(void);
uint32_t esp_vision_display_get_height(void);
uint32_t esp_vision_display_get_backlight(void);
esp_err_t esp_vision_display_set_backlight(uint32_t backlight);
esp_err_t esp_vision_display_clear(bool display_off);
esp_err_t esp_vision_display_write(const image_t *img, const esp_vision_display_draw_config_t *config);
// Borrow the last RGB565 frame submitted to the panel. Call on the VM task;
// the view expires on the next display write, clear or deinit. No panel readback.
esp_err_t esp_vision_display_get_frame(image_t *frame);

#ifdef __cplusplus
}
#endif

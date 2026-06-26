/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct image image_t;

typedef enum {
    ESP_VISION_CAMERA_FRAMESIZE_QQVGA = 0,
    ESP_VISION_CAMERA_FRAMESIZE_QVGA,
    ESP_VISION_CAMERA_FRAMESIZE_VGA,
} esp_vision_camera_framesize_t;

typedef struct {
    bool ready;
    uint32_t sensor_id;
    uint32_t raw_input_width;
    uint32_t raw_input_height;
    uint32_t active_input_width;
    uint32_t active_input_height;
    uint32_t active_input_offset_x;
    uint32_t active_input_offset_y;
    uint32_t width;
    uint32_t height;
    uint32_t pixfmt;
    bool hmirror;
    bool vflip;
} esp_vision_camera_status_t;

void esp_vision_camera_init0(void);
esp_err_t esp_vision_camera_init(void);
void esp_vision_camera_deinit(void);
bool esp_vision_camera_is_ready(void);

esp_err_t esp_vision_camera_set_pixformat(uint32_t pixfmt);
uint32_t esp_vision_camera_get_pixformat(void);

esp_err_t esp_vision_camera_set_framesize(esp_vision_camera_framesize_t framesize);
esp_err_t esp_vision_camera_get_framesize_dimensions(esp_vision_camera_framesize_t framesize,
                                                     uint32_t *width,
                                                     uint32_t *height);
uint32_t esp_vision_camera_get_width(void);
uint32_t esp_vision_camera_get_height(void);

esp_err_t esp_vision_camera_set_hmirror(bool enable);
bool esp_vision_camera_get_hmirror(void);
esp_err_t esp_vision_camera_set_vflip(bool enable);
bool esp_vision_camera_get_vflip(void);

uint32_t esp_vision_camera_get_sensor_id(void);
size_t esp_vision_camera_frame_size(void);
esp_err_t esp_vision_camera_capture(uint8_t *pixels, size_t pixels_size);
esp_err_t esp_vision_camera_snapshot(image_t *img, uint8_t *pixels, size_t pixels_size);
esp_err_t esp_vision_camera_get_status(esp_vision_camera_status_t *status);

#ifdef __cplusplus
}
#endif

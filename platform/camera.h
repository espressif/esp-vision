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
esp_err_t esp_vision_camera_shutdown(void);
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
// Camera state, capture and observed views belong to the main VM task. Startup
// binds the task before camera init; Python worker tasks must not mutate it.
void esp_vision_camera_framebuffer_init0(void);
bool esp_vision_camera_is_vm_task(void);
// Application snapshot storage is allocated on first use and borrowed until
// the next application capture/reconfiguration/reset. No storage is reserved at boot.
esp_err_t esp_vision_camera_get_snapshot_buffer(size_t size, uint8_t **pixels);
void esp_vision_camera_release_snapshot_buffer(void);
// A successful application capture publishes an independent, immutable copy.
// Observation allocation failure does not fail the application capture.
esp_err_t esp_vision_camera_capture(uint8_t *pixels, size_t pixels_size);
// Board backend: fill caller storage without publishing or allocating a view.
esp_err_t esp_vision_camera_capture_into(uint8_t *pixels, size_t pixels_size);
// Borrowed observed pixels are valid only in this VM call until reconfiguration
// or another application capture. The observer never initializes/captures.
esp_err_t esp_vision_camera_get_observed_frame(image_t *img);
void esp_vision_camera_invalidate_observed_frame(void);
esp_err_t esp_vision_camera_snapshot(image_t *img, uint8_t *pixels, size_t pixels_size);
esp_err_t esp_vision_camera_get_status(esp_vision_camera_status_t *status);

#ifdef __cplusplus
}
#endif

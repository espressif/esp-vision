/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera.h"

#include <string.h>

void esp_vision_camera_init0(void)
{
    if (!esp_vision_camera_is_vm_task()) {
        return;
    }
    esp_vision_camera_invalidate_observed_frame();
}

esp_err_t esp_vision_camera_init(void)
{
    if (!esp_vision_camera_is_vm_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void esp_vision_camera_deinit(void)
{
    if (!esp_vision_camera_is_vm_task()) {
        return;
    }
    esp_vision_camera_invalidate_observed_frame();
}

bool esp_vision_camera_is_ready(void)
{
    return false;
}

esp_err_t esp_vision_camera_set_pixformat(uint32_t pixfmt)
{
    if (!esp_vision_camera_is_vm_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixfmt != esp_vision_camera_get_pixformat()) {
        esp_vision_camera_invalidate_observed_frame();
    }
    (void)pixfmt;
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t esp_vision_camera_get_pixformat(void)
{
    return 0;
}

esp_err_t esp_vision_camera_set_framesize(esp_vision_camera_framesize_t framesize)
{
    if (!esp_vision_camera_is_vm_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t observed_width = 0;
    uint32_t observed_height = 0;
    if ((esp_vision_camera_get_framesize_dimensions(framesize, &observed_width, &observed_height) == ESP_OK) &&
            ((observed_width != esp_vision_camera_get_width()) ||
             (observed_height != esp_vision_camera_get_height()))) {
        esp_vision_camera_invalidate_observed_frame();
    }
    (void)framesize;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_vision_camera_get_framesize_dimensions(esp_vision_camera_framesize_t framesize,
                                                     uint32_t *width,
                                                     uint32_t *height)
{
    if ((width == NULL) || (height == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (framesize) {
    case ESP_VISION_CAMERA_FRAMESIZE_QQVGA:
        *width = 160;
        *height = 120;
        return ESP_OK;
    case ESP_VISION_CAMERA_FRAMESIZE_QVGA:
        *width = 320;
        *height = 240;
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

uint32_t esp_vision_camera_get_width(void)
{
    return 0;
}

uint32_t esp_vision_camera_get_height(void)
{
    return 0;
}

esp_err_t esp_vision_camera_set_hmirror(bool enable)
{
    if (!esp_vision_camera_is_vm_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable != esp_vision_camera_get_hmirror()) {
        esp_vision_camera_invalidate_observed_frame();
    }
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

bool esp_vision_camera_get_hmirror(void)
{
    return false;
}

esp_err_t esp_vision_camera_set_vflip(bool enable)
{
    if (!esp_vision_camera_is_vm_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable != esp_vision_camera_get_vflip()) {
        esp_vision_camera_invalidate_observed_frame();
    }
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

bool esp_vision_camera_get_vflip(void)
{
    return false;
}

uint32_t esp_vision_camera_get_sensor_id(void)
{
    return 0;
}

size_t esp_vision_camera_frame_size(void)
{
    return 0;
}

esp_err_t esp_vision_camera_capture_into(uint8_t *pixels, size_t pixels_size)
{
    if (!esp_vision_camera_is_vm_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)pixels;
    (void)pixels_size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_vision_camera_snapshot(image_t *img, uint8_t *pixels, size_t pixels_size)
{
    (void)img;
    (void)pixels;
    (void)pixels_size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_vision_camera_get_status(esp_vision_camera_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    return ESP_OK;
}

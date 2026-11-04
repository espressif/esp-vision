/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "ev_memory.h"

#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif
#include "imlib.h"

static TaskHandle_t s_camera_vm_task;
static uint8_t *s_snapshot;
static size_t s_snapshot_capacity;
static image_t s_observed_frame;
static size_t s_observed_capacity;
static esp_err_t s_observed_result = ESP_ERR_INVALID_STATE;

bool esp_vision_camera_is_vm_task(void)
{
    return (s_camera_vm_task != NULL) && (xTaskGetCurrentTaskHandle() == s_camera_vm_task);
}

void esp_vision_camera_invalidate_observed_frame(void)
{
    if (!esp_vision_camera_is_vm_task()) {
        return;
    }
    heap_caps_free(s_observed_frame.pixels);
    memset(&s_observed_frame, 0, sizeof(s_observed_frame));
    s_observed_capacity = 0;
    s_observed_result = ESP_ERR_INVALID_STATE;
}

void esp_vision_camera_release_snapshot_buffer(void)
{
    if (!esp_vision_camera_is_vm_task()) {
        return;
    }
    esp_vision_camera_invalidate_observed_frame();
    heap_caps_free(s_snapshot);
    s_snapshot = NULL;
    s_snapshot_capacity = 0;
}

void esp_vision_camera_framebuffer_init0(void)
{
    // Called by main.c before camera initialization, also after a VM soft reset.
    // Never let a Python worker task acquire camera state from the main task.
    if (s_camera_vm_task == NULL) {
        s_camera_vm_task = xTaskGetCurrentTaskHandle();
    }
    esp_vision_camera_release_snapshot_buffer();
}

esp_err_t esp_vision_camera_get_snapshot_buffer(size_t size, uint8_t **pixels)
{
    if ((pixels == NULL) || (size == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    *pixels = NULL;
    if (!esp_vision_camera_is_vm_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_snapshot_capacity < size) {
        uint8_t *next = esp_vision_memory_malloc(size, MALLOC_CAP_8BIT);
        if (next == NULL) {
            return ESP_ERR_NO_MEM;
        }
        heap_caps_free(s_snapshot);
        s_snapshot = next;
        s_snapshot_capacity = size;
    }
    *pixels = s_snapshot;
    return ESP_OK;
}

esp_err_t esp_vision_camera_capture(uint8_t *pixels, size_t pixels_size)
{
    if (!esp_vision_camera_is_vm_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    // Failed acquisition may have partially touched the application's output;
    // do not let an earlier publication masquerade as its replacement.
    if (s_observed_result != ESP_ERR_NO_MEM) {
        s_observed_result = ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = esp_vision_camera_capture_into(pixels, pixels_size);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t size = esp_vision_camera_frame_size();
    uint32_t width = esp_vision_camera_get_width();
    uint32_t height = esp_vision_camera_get_height();
    uint32_t pixfmt = esp_vision_camera_get_pixformat();
    size_t bpp = (pixfmt == PIXFORMAT_RGB565) ? 2 : (pixfmt == PIXFORMAT_GRAYSCALE) ? 1 : 0;
    if ((pixels == NULL) || (size == 0) || (size > pixels_size) || (size > UINT32_MAX) ||
            (width == 0) || (height == 0) || (width > INT32_MAX) || (height > INT32_MAX) ||
            (bpp == 0) || (width > (SIZE_MAX / height / bpp)) ||
            (size != (size_t)width * height * bpp)) {
        // A malformed backend result cannot publish out-of-bounds memory.
        s_observed_result = ESP_ERR_INVALID_SIZE;
        return ESP_OK;
    }
    if (s_observed_result == ESP_ERR_NO_MEM) {
        // Retry after reconfiguration/reset, not malloc on every camera frame.
        return ESP_OK;
    }
    if (s_observed_capacity < size) {
        uint8_t *next = esp_vision_memory_malloc_preferred(size, MALLOC_CAP_8BIT,
                                                           ESP_VISION_MEMORY_PREFER_SPIRAM);
        if (next == NULL) {
            s_observed_result = ESP_ERR_NO_MEM;
            return ESP_OK;
        }
        heap_caps_free(s_observed_frame.pixels);
        s_observed_frame.pixels = next;
        s_observed_capacity = size;
    }
    // Capture backends return only after DMA/PPA completion and cache sync.
    // This CPU-owned copy never borrows GC-managed user buffers or DMA buffers.
    memcpy(s_observed_frame.pixels, pixels, size);
    s_observed_frame.w = (int32_t)width;
    s_observed_frame.h = (int32_t)height;
    s_observed_frame.pixfmt = pixfmt;
    s_observed_frame.size = size;
    s_observed_frame._raw = NULL;
    s_observed_result = ESP_OK;
    return ESP_OK;
}

esp_err_t esp_vision_camera_get_observed_frame(image_t *img)
{
    if (img == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(img, 0, sizeof(*img));
    if (!esp_vision_camera_is_vm_task() || !esp_vision_camera_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_observed_result != ESP_OK) {
        return s_observed_result;
    }
    // The RPC dispatcher and camera mutators run on the same VM task. JPEG
    // encoding completes in that call; neither Python scheduling nor transport
    // cancellation can recycle the borrowed pixels before it finishes.
    *img = s_observed_frame;
    return ESP_OK;
}

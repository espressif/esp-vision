/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera.h"
#include "esp_heap_caps.h"

static uint8_t *s_snapshot;
static size_t s_snapshot_capacity;

void esp_vision_camera_release_snapshot_buffer(void)
{
    heap_caps_free(s_snapshot);
    s_snapshot = NULL;
    s_snapshot_capacity = 0;
}

esp_err_t esp_vision_camera_get_snapshot_buffer(size_t size, uint8_t **pixels)
{
    if ((pixels == NULL) || (size == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    *pixels = NULL;
    if (s_snapshot_capacity < size) {
        uint8_t *next = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (next == NULL) {
            next = heap_caps_malloc(size, MALLOC_CAP_8BIT);
        }
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

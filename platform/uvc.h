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

typedef struct image image_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t interval_us;
} esp_vision_uvc_mode_t;

typedef void (*esp_vision_uvc_mode_committed_cb_t)(const esp_vision_uvc_mode_t *mode);

esp_err_t esp_vision_uvc_init(void);
void esp_vision_uvc_reset(void);
bool esp_vision_uvc_streaming(void);
void esp_vision_uvc_get_mode(esp_vision_uvc_mode_t *mode);
// The callback is a C notification under the USB lock. It can queue work but
// must not invoke Python. Registration replays the last valid host commit.
void esp_vision_uvc_set_mode_committed_cb(esp_vision_uvc_mode_committed_cb_t callback);
void esp_vision_uvc_bind_preview(bool enabled);
// A nonzero epoch allows preview to encode before trying to publish. Only the
// same still-bound stream can accept that JPEG after encoding finishes.
uint32_t esp_vision_uvc_preview_epoch(void);
// The bound preview can wait briefly to replace its in-flight output frame.
esp_err_t esp_vision_uvc_publish(uint32_t epoch, const uint8_t *jpeg, size_t size, bool *accepted);
esp_err_t esp_vision_uvc_write(const image_t *img, bool detach_preview, bool *accepted);

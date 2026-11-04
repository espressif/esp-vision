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

#define ESP_VISION_UVC_WIDTH (320)
#define ESP_VISION_UVC_HEIGHT (240)
#define ESP_VISION_UVC_INTERVAL_US (100000)
#define ESP_VISION_UVC_MAX_FRAME (48 * 1024)

esp_err_t esp_vision_uvc_init(void);
void esp_vision_uvc_reset(void);
bool esp_vision_uvc_streaming(void);
void esp_vision_uvc_bind_preview(bool enabled);
bool esp_vision_uvc_preview_ready(const image_t *img);
esp_err_t esp_vision_uvc_publish(const uint8_t *jpeg, size_t size, bool fixed, bool *accepted);
esp_err_t esp_vision_uvc_write(const image_t *img, bool fixed, bool *accepted);

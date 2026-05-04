/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct image image_t;

void esp_vision_jpeg_deinit(void);
esp_err_t esp_vision_jpeg_encode(const image_t *img, int quality, uint8_t **jpeg_buf, size_t *jpeg_size);

#ifdef __cplusplus
}
#endif

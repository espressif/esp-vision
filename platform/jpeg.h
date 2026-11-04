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
esp_err_t esp_vision_jpeg_decode(const uint8_t *jpeg_buf, size_t jpeg_size, image_t *dst);
// On failure available caller outputs are reset to NULL/0. A successful encode
// passes bounded JPEG structure/table/dimension checks; no full entropy decode.
// Encoded raw-image output is module-owned until the next encode or deinit
// and may be CPU-only memory without hardware DMA alignment. JPEG input is
// returned as a validated borrowed view: its lifetime belongs to the source
// Image. Asynchronous consumers must copy either form before returning.
esp_err_t esp_vision_jpeg_encode(const image_t *img, int quality, uint8_t **jpeg_buf, size_t *jpeg_size);

#ifdef __cplusplus
}
#endif

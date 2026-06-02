// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ESP_VISION_ZXING_H
#define ESP_VISION_ZXING_H

#if defined(__IMLIB_H__)
#include "imlib.h"
#else
#include "esp_vision_zxing_imlib_types.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

void esp_vision_zxing_find_barcodes(list_t *out, image_t *img, rectangle_t *roi);

#ifdef __cplusplus
}
#endif

#endif // ESP_VISION_ZXING_H

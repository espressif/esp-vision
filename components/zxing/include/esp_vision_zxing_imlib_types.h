// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ESP_VISION_ZXING_IMLIB_TYPES_H
#define ESP_VISION_ZXING_IMLIB_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "collections.h"

#ifdef __cplusplus
}
#endif

typedef struct point {
    int16_t x;
    int16_t y;
} point_t;

typedef struct rectangle {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rectangle_t;

#define PIXFORMAT_FLAGS_M         (1 << 27)
#define PIXFORMAT_ID_GRAY         2
#define SUBFORMAT_ID_GRAY8        0
#define PIXFORMAT_BPP_GRAY8       1

typedef enum pixformat {
    PIXFORMAT_GRAYSCALE = (PIXFORMAT_FLAGS_M | (PIXFORMAT_ID_GRAY << 16) | (SUBFORMAT_ID_GRAY8 << 8) | PIXFORMAT_BPP_GRAY8),
} pixformat_t;

typedef struct image {
    int32_t w;
    int32_t h;
    uint32_t pixfmt;
    uint32_t size;
    uint8_t *_raw;
    union {
        uint8_t *pixels;
        uint8_t *data;
    };
} image_t;

#define IMAGE_COMPUTE_GRAYSCALE_PIXEL_ROW_PTR(image, y) (((uint8_t *) (image)->data) + ((image)->w * (y)))
#define IM_IS_GS(img)                                    ((img)->pixfmt == PIXFORMAT_GRAYSCALE)

typedef enum barcodes {
    BARCODE_EAN2,
    BARCODE_EAN5,
    BARCODE_EAN8,
    BARCODE_UPCE,
    BARCODE_ISBN10,
    BARCODE_UPCA,
    BARCODE_EAN13,
    BARCODE_ISBN13,
    BARCODE_I25,
    BARCODE_DATABAR,
    BARCODE_DATABAR_EXP,
    BARCODE_CODABAR,
    BARCODE_CODE39,
    BARCODE_PDF417,
    BARCODE_CODE93,
    BARCODE_CODE128
} barcodes_t;

typedef struct find_barcodes_list_lnk_data {
    point_t corners[4];
    rectangle_t rect;
    size_t payload_len;
    char *payload;
    uint16_t type, rotation;
    int quality;
} find_barcodes_list_lnk_data_t;

#endif // ESP_VISION_ZXING_IMLIB_TYPES_H

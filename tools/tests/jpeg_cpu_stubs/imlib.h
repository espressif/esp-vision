// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#define OMV_JPEG_CODEC_ENABLE 0
#define PIXFORMAT_BINARY 0
#define PIXFORMAT_GRAYSCALE 1
#define PIXFORMAT_RGB565 (2u | (1u << 26))
#define PIXFORMAT_JPEG (5u | (1u << 25))
#define PIXFORMAT_YUV422 3
#define PIXFORMAT_YUV_ANY 3
#define PIXFORMAT_BAYER_ANY 4
#define JPEG_MCU_W 8
#define JPEG_MCU_H 8
#define JPEG_444_GS_MCU_SIZE 64
#define JPEG_MAX_ALLOC_SIZE (1024u*1024u)
#define IMLIB_IMAGE_MAX_SIZE(x) (x)
#define FB_ALLOC_PREFER_SIZE 2
#define FB_ALLOC_CACHE_ALIGN 4
#define UINT32_T_SHIFT 5
#define UINT32_T_MASK 31
#define IM_MIN(a,b) ((a)<(b)?(a):(b))
typedef enum {JPEG_SUBSAMPLING_AUTO = 0, JPEG_SUBSAMPLING_444 = 0x11, JPEG_SUBSAMPLING_422 = 0x21, JPEG_SUBSAMPLING_420 = 0x22} jpeg_subsampling_t;
typedef struct image {
    int32_t w, h;
    union {
        uint32_t pixfmt;
        struct {
            uint32_t unused: 25, is_compressed: 1, is_color: 1, reserved: 5;
        };
    };
    uint32_t size;
    union {
        uint8_t *pixels;
        uint8_t *data;
    };
} image_t;
typedef struct {
    int x, y, w, h;
} rectangle_t;
#define IMAGE_COMPUTE_BINARY_PIXEL_ROW_PTR(src,y) ((uint32_t*)(src)->data+(((src)->w+31)/32)*(y))
#define IMAGE_COMPUTE_GRAYSCALE_PIXEL_ROW_PTR(src,y) ((src)->data+(src)->w*(y))
#define IMAGE_COMPUTE_RGB565_PIXEL_ROW_PTR(src,y) ((uint16_t*)(src)->data+(src)->w*(y))
#define IMAGE_COMPUTE_YUV_PIXEL_ROW_PTR(src,y) IMAGE_COMPUTE_RGB565_PIXEL_ROW_PTR(src,y)
#define COLOR_RGB565_TO_R8(p) ((((p)>>8)&0xf8)|(((p)>>13)&7))
#define COLOR_RGB565_TO_G8(p) ((((p)>>3)&0xfc)|(((p)>>9)&3))
#define COLOR_RGB565_TO_B8(p) ((((p)<<3)&0xf8)|(((p)>>2)&7))
#define COLOR_RGB888_TO_Y(r,g,b) (((r)*38+(g)*75+(b)*15)>>7)
#define COLOR_RGB888_TO_U(r,g,b) (((r)*-21-(g)*43+(b)*64)>>7)
#define COLOR_RGB888_TO_V(r,g,b) (((r)*64-(g)*54-(b)*10)>>7)
#define fast_roundf(x) ((int)lrintf(x))
#define __CLZ(x) ((x)?__builtin_clz((uint32_t)(x)):32)
static inline uint32_t __SSUB16(uint32_t a, uint32_t b)
{
    uint16_t low = (uint16_t)((int16_t)a - (int16_t)b);
    uint16_t high = (uint16_t)((int16_t)(a >> 16) - (int16_t)(b >> 16));
    return ((uint32_t)high << 16) | low;
}
static inline void *m_realloc(void *p, size_t n)
{
    (void)p;
    (void)n;
    fputs("FORBIDDEN m_realloc\n", stderr);
    abort();
}
static inline uint32_t fb_avail(void)
{
    fputs("FORBIDDEN fb_avail\n", stderr);
    abort();
}
static inline void *fb_alloc(uint32_t n, int hints)
{
    (void)n;
    (void)hints;
    fputs("FORBIDDEN fb_alloc\n", stderr);
    abort();
}
static inline void imlib_debayer_ycbcr(image_t *s, rectangle_t *r, int8_t *y, int8_t *u, int8_t *v)
{
    (void)s;
    (void)r;
    (void)y;
    (void)u;
    (void)v;
    abort();
}
bool jpeg_compress(image_t*, image_t*, int, bool, jpeg_subsampling_t);

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

// Opaque hardware H.264 encoder session. One instance per video stream:
// open() once, encode() per frame, close() to release.
typedef struct esp_vision_h264_enc esp_vision_h264_enc_t;

// Frame type reported alongside each encoded NAL (mirrors esp_h264_frame_type_t).
#define ESP_VISION_H264_FRAME_INVALID (-1)
#define ESP_VISION_H264_FRAME_IDR     (0)
#define ESP_VISION_H264_FRAME_I       (1)
#define ESP_VISION_H264_FRAME_P       (2)

// Create a hardware H.264 encoder. width/height must be multiples of 16 and
// within the hardware range (80..1920 x 80..2032). bitrate is the target in
// bits/s; qp_min/qp_max bound the quantization parameter (0..51). On success
// *out_enc owns all encoder resources until esp_vision_h264_close().
// Returns ESP_ERR_NOT_SUPPORTED on targets without the esp_h264 component.
esp_err_t esp_vision_h264_open(int width, int height, int fps, int gop,
                               int bitrate, int qp_min, int qp_max,
                               esp_vision_h264_enc_t **out_enc);

// Encode one RGB565 frame. img dimensions must match the open() resolution.
// On success *nal points to encoder-owned memory valid until the next
// encode()/close() call, *nal_len is its length, and *frame_type is one of the
// ESP_VISION_H264_FRAME_* values.
esp_err_t esp_vision_h264_encode(esp_vision_h264_enc_t *enc, const image_t *img,
                                 uint8_t **nal, size_t *nal_len, int *frame_type);

// Release the encoder and its buffers. Safe to call with NULL.
void esp_vision_h264_close(esp_vision_h264_enc_t *enc);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// This file is only compiled on ESP32-P4 (see micropython.cmake), where the
// esp_h264 component is pulled in as a private dependency of esp_video.

#include "h264.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif

#ifndef NO_QSTR
#include "imlib.h"
#endif

#include "esp_h264_alloc.h"
#include "esp_h264_enc_single_hw.h"

struct esp_vision_h264_enc {
    esp_h264_enc_handle_t enc;
    int w;
    int h;
    uint8_t *in_buf;        // 16-byte / cache-aligned, DMA-capable RGB565 input
    uint32_t in_buf_size;   // actual allocation size (cache-line aligned)
    size_t raw_size;        // valid input bytes per frame (w * h * 2)
    uint8_t *out_buf;       // encoded NAL output
    uint32_t out_buf_size;  // actual allocation size
    uint32_t pts;           // monotonically increasing presentation timestamp
};

static esp_err_t esp_vision_h264_err_to_esp_err(esp_h264_err_t err)
{
    switch (err) {
    case ESP_H264_ERR_OK:
        return ESP_OK;
    case ESP_H264_ERR_ARG:
        return ESP_ERR_INVALID_ARG;
    case ESP_H264_ERR_MEM:
        return ESP_ERR_NO_MEM;
    case ESP_H264_ERR_UNSUPPORTED:
        return ESP_ERR_NOT_SUPPORTED;
    case ESP_H264_ERR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case ESP_H264_ERR_OVERFLOW:
        return ESP_ERR_INVALID_SIZE;
    case ESP_H264_ERR_FAIL:
    default:
        return ESP_FAIL;
    }
}

void esp_vision_h264_close(esp_vision_h264_enc_t *enc)
{
    if (enc == NULL) {
        return;
    }
    if (enc->enc != NULL) {
        esp_h264_enc_close(enc->enc);
        esp_h264_enc_del(enc->enc);
        enc->enc = NULL;
    }
    if (enc->in_buf != NULL) {
        esp_h264_free(enc->in_buf);
        enc->in_buf = NULL;
    }
    if (enc->out_buf != NULL) {
        esp_h264_free(enc->out_buf);
        enc->out_buf = NULL;
    }
    free(enc);
}

esp_err_t esp_vision_h264_open(int width, int height, int fps, int gop,
                               int bitrate, int qp_min, int qp_max,
                               esp_vision_h264_enc_t **out_enc)
{
    if (out_enc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_enc = NULL;

    // Hardware encoder constraints: 16-aligned, 80..1920 x 80..2032.
    if ((width <= 0) || (height <= 0) || ((width % 16) != 0) || ((height % 16) != 0) ||
            (width < 80) || (width > 1920) || (height < 80) || (height > 2032)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((fps <= 0) || (fps > 255) || (gop <= 0) || (gop > 255) || (bitrate <= 0) ||
            (qp_min < 0) || (qp_max > ESP_H264_QP_MAX) || (qp_min > qp_max)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t raw_size = (size_t)width * (size_t)height * sizeof(uint16_t);
    if (raw_size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_vision_h264_enc_t *self = calloc(1, sizeof(*self));
    if (self == NULL) {
        return ESP_ERR_NO_MEM;
    }
    self->w = width;
    self->h = height;
    self->raw_size = raw_size;

    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_RGB565_LE,   // fed straight from image_t (P4 rev >= 3.0)
        .gop = (uint8_t)gop,
        .fps = (uint8_t)fps,
        .res = {
            .width = (uint16_t)width,
            .height = (uint16_t)height,
        },
        .rc = {
            .bitrate = (uint32_t)bitrate,
            .qp_min = (uint8_t)qp_min,
            .qp_max = (uint8_t)qp_max,
        },
    };

    esp_h264_err_t h264_ret = esp_h264_enc_hw_new(&cfg, &self->enc);
    if (h264_ret != ESP_H264_ERR_OK) {
        esp_vision_h264_close(self);
        return esp_vision_h264_err_to_esp_err(h264_ret);
    }
    h264_ret = esp_h264_enc_open(self->enc);
    if (h264_ret != ESP_H264_ERR_OK) {
        esp_vision_h264_close(self);
        return esp_vision_h264_err_to_esp_err(h264_ret);
    }

    // DMA-capable, cache-aligned input; the encoder reads it over DMA.
    self->in_buf = esp_h264_aligned_calloc(16, 1, (uint32_t)raw_size, &self->in_buf_size,
                                           ESP_H264_MEM_SPIRAM);
    // Output capacity: encoded frame never exceeds the raw input size.
    self->out_buf = esp_h264_aligned_calloc(16, 1, (uint32_t)raw_size, &self->out_buf_size,
                                            ESP_H264_MEM_SPIRAM);
    if ((self->in_buf == NULL) || (self->out_buf == NULL)) {
        esp_vision_h264_close(self);
        return ESP_ERR_NO_MEM;
    }

    *out_enc = self;
    return ESP_OK;
}

esp_err_t esp_vision_h264_encode(esp_vision_h264_enc_t *enc, const image_t *img,
                                 uint8_t **nal, size_t *nal_len, int *frame_type)
{
    if ((enc == NULL) || (nal == NULL) || (nal_len == NULL) || (img == NULL) ||
            (img->pixels == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (img->pixfmt != PIXFORMAT_RGB565) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((img->w != enc->w) || (img->h != enc->h)) {
        return ESP_ERR_INVALID_SIZE;
    }

    // The hardware encoder driver does the cache writeback (input) and
    // invalidate (output) around its own DMA internally, so no manual cache
    // maintenance is needed here for these (cacheable PSRAM) buffers.
    memcpy(enc->in_buf, img->pixels, enc->raw_size);

    esp_h264_enc_in_frame_t in_frame = {
        .raw_data = {
            .buffer = enc->in_buf,
            .len = (uint32_t)enc->raw_size,
        },
        .pts = enc->pts,
    };
    esp_h264_enc_out_frame_t out_frame = {
        .raw_data = {
            .buffer = enc->out_buf,
            .len = enc->out_buf_size,
        },
    };

    esp_h264_err_t h264_ret = esp_h264_enc_process(enc->enc, &in_frame, &out_frame);
    if (h264_ret != ESP_H264_ERR_OK) {
        return esp_vision_h264_err_to_esp_err(h264_ret);
    }

    enc->pts++;
    *nal = out_frame.raw_data.buffer;
    *nal_len = out_frame.length;
    if (frame_type != NULL) {
        *frame_type = (int)out_frame.frame_type;
    }
    return ESP_OK;
}

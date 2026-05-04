/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_vision_jpeg.h"

#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_encode.h"

#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif

#ifndef NO_QSTR
#include "imlib.h"
#endif

#define ESP_VISION_JPEG_TIMEOUT_MS (200)

typedef struct {
    jpeg_encoder_handle_t encoder;
    uint8_t *input_buf;
    size_t input_buf_size;
    uint8_t *jpeg_buf;
    size_t jpeg_buf_size;
} esp_vision_jpeg_context_t;

static esp_vision_jpeg_context_t s_jpeg;

static void esp_vision_jpeg_free_buffers(void)
{
    if (s_jpeg.input_buf != NULL) {
        free(s_jpeg.input_buf);
        s_jpeg.input_buf = NULL;
    }
    if (s_jpeg.jpeg_buf != NULL) {
        free(s_jpeg.jpeg_buf);
        s_jpeg.jpeg_buf = NULL;
    }
    s_jpeg.input_buf_size = 0;
    s_jpeg.jpeg_buf_size = 0;
}

void esp_vision_jpeg_deinit(void)
{
    if (s_jpeg.encoder != NULL) {
        jpeg_del_encoder_engine(s_jpeg.encoder);
        s_jpeg.encoder = NULL;
    }
    esp_vision_jpeg_free_buffers();
}

static esp_err_t esp_vision_jpeg_ensure_encoder(void)
{
    if (s_jpeg.encoder != NULL) {
        return ESP_OK;
    }

    jpeg_encode_engine_cfg_t encoder_config = {
        .intr_priority = 0,
        .timeout_ms = ESP_VISION_JPEG_TIMEOUT_MS,
    };

    return jpeg_new_encoder_engine(&encoder_config, &s_jpeg.encoder);
}

static size_t esp_vision_jpeg_capacity(size_t raw_size)
{
    size_t capacity = raw_size + (raw_size / 2) + 4096;
    return (capacity < 16384) ? 16384 : capacity;
}

static esp_err_t esp_vision_jpeg_ensure_buffers(size_t raw_size)
{
    if (s_jpeg.input_buf_size < raw_size) {
        if (s_jpeg.input_buf != NULL) {
            free(s_jpeg.input_buf);
            s_jpeg.input_buf = NULL;
            s_jpeg.input_buf_size = 0;
        }

        jpeg_encode_memory_alloc_cfg_t input_config = {
            .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
        };
        s_jpeg.input_buf = jpeg_alloc_encoder_mem(raw_size, &input_config, &s_jpeg.input_buf_size);
        if (s_jpeg.input_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    size_t jpeg_capacity = esp_vision_jpeg_capacity(raw_size);
    if (s_jpeg.jpeg_buf_size < jpeg_capacity) {
        if (s_jpeg.jpeg_buf != NULL) {
            free(s_jpeg.jpeg_buf);
            s_jpeg.jpeg_buf = NULL;
            s_jpeg.jpeg_buf_size = 0;
        }

        jpeg_encode_memory_alloc_cfg_t output_config = {
            .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
        };
        s_jpeg.jpeg_buf = jpeg_alloc_encoder_mem(jpeg_capacity, &output_config, &s_jpeg.jpeg_buf_size);
        if (s_jpeg.jpeg_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static esp_err_t esp_vision_jpeg_image_format(const image_t *img,
                                              size_t *raw_size,
                                              jpeg_enc_input_format_t *src_type,
                                              jpeg_down_sampling_type_t *sub_sample)
{
    if ((img == NULL) || (img->pixels == NULL) || (img->w <= 0) || (img->h <= 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (img->pixfmt) {
    case PIXFORMAT_GRAYSCALE:
        *raw_size = (size_t)img->w * (size_t)img->h;
        *src_type = JPEG_ENCODE_IN_FORMAT_GRAY;
        *sub_sample = JPEG_DOWN_SAMPLING_GRAY;
        return ESP_OK;
    case PIXFORMAT_RGB565:
        *raw_size = (size_t)img->w * (size_t)img->h * sizeof(uint16_t);
        *src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
        *sub_sample = JPEG_DOWN_SAMPLING_YUV422;
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t esp_vision_jpeg_encode(const image_t *img, int quality, uint8_t **jpeg_buf, size_t *jpeg_size)
{
    if ((jpeg_buf == NULL) || (jpeg_size == NULL) || (quality < 1) || (quality > 100)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t raw_size = 0;
    jpeg_enc_input_format_t src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    jpeg_down_sampling_type_t sub_sample = JPEG_DOWN_SAMPLING_YUV422;

    esp_err_t ret = esp_vision_jpeg_image_format(img, &raw_size, &src_type, &sub_sample);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_vision_jpeg_ensure_encoder();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_vision_jpeg_ensure_buffers(raw_size);
    if (ret != ESP_OK) {
        return ret;
    }

    memcpy(s_jpeg.input_buf, img->pixels, raw_size);

    jpeg_encode_cfg_t encode_config = {
        .height = (uint32_t)img->h,
        .width = (uint32_t)img->w,
        .src_type = src_type,
        .sub_sample = sub_sample,
        .image_quality = quality,
    };

    uint32_t out_size = 0;
    ret = jpeg_encoder_process(s_jpeg.encoder,
                               &encode_config,
                               s_jpeg.input_buf,
                               (uint32_t)raw_size,
                               s_jpeg.jpeg_buf,
                               (uint32_t)s_jpeg.jpeg_buf_size,
                               &out_size);
    if (ret != ESP_OK) {
        return ret;
    }

    *jpeg_buf = s_jpeg.jpeg_buf;
    *jpeg_size = out_size;
    return ESP_OK;
}

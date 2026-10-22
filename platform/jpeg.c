/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "jpeg.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "soc/soc_caps.h"

#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif

#ifndef NO_QSTR
#include "imlib.h"
#endif

#include "ev_memory.h"

#ifndef SOC_JPEG_ENCODE_SUPPORTED
#define SOC_JPEG_ENCODE_SUPPORTED 0
#endif

#ifndef __has_include
#define ESP_VISION_HAS_ESP_NEW_JPEG 0
#elif __has_include("esp_jpeg_dec.h")
#define ESP_VISION_HAS_ESP_NEW_JPEG 1
#define jpeg_subsampling_t esp_new_jpeg_subsampling_t
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"
#undef jpeg_subsampling_t
#else
#define ESP_VISION_HAS_ESP_NEW_JPEG 0
#endif

#define ESP_VISION_HAS_NEW_JPEG_ENCODER (ESP_VISION_HAS_ESP_NEW_JPEG && !SOC_JPEG_ENCODE_SUPPORTED)

#if ESP_VISION_HAS_ESP_NEW_JPEG

static esp_err_t esp_vision_jpeg_error_to_esp_err(jpeg_error_t err)
{
    switch (err) {
    case JPEG_ERR_OK:
        return ESP_OK;
    case JPEG_ERR_NO_MEM:
        return ESP_ERR_NO_MEM;
    case JPEG_ERR_INVALID_PARAM:
        return ESP_ERR_INVALID_ARG;
    case JPEG_ERR_BAD_DATA:
    case JPEG_ERR_NO_MORE_DATA:
        return ESP_ERR_INVALID_RESPONSE;
    case JPEG_ERR_UNSUPPORT_FMT:
    case JPEG_ERR_UNSUPPORT_STD:
        return ESP_ERR_NOT_SUPPORTED;
    case JPEG_ERR_FAIL:
    default:
        return ESP_FAIL;
    }
}

static void esp_vision_jpeg_rgb565_to_grayscale(const uint16_t *src, uint8_t *dst, size_t pixel_count)
{
    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t pixel = src[i];
        dst[i] = COLOR_RGB565_TO_GRAYSCALE(pixel);
    }
}

static esp_err_t esp_vision_jpeg_open_decoder(jpeg_dec_config_t *config,
                                              const uint8_t *jpeg_buf,
                                              size_t jpeg_size,
                                              jpeg_dec_handle_t *decoder,
                                              jpeg_dec_io_t *io,
                                              jpeg_dec_header_info_t *info)
{
    *decoder = NULL;
    jpeg_error_t jpeg_ret = jpeg_dec_open(config, decoder);
    if (jpeg_ret != JPEG_ERR_OK) {
        return esp_vision_jpeg_error_to_esp_err(jpeg_ret);
    }

    *io = (jpeg_dec_io_t) {
        .inbuf = (uint8_t *)jpeg_buf,
        .inbuf_len = (int)jpeg_size,
    };

    jpeg_ret = jpeg_dec_parse_header(*decoder, io, info);
    if (jpeg_ret != JPEG_ERR_OK) {
        jpeg_dec_close(*decoder);
        *decoder = NULL;
        return esp_vision_jpeg_error_to_esp_err(jpeg_ret);
    }

    return ESP_OK;
}

#if ESP_VISION_HAS_NEW_JPEG_ENCODER

typedef struct {
    uint8_t *input_buf;
    size_t input_buf_size;
    uint8_t *jpeg_buf;
    size_t jpeg_buf_size;
} esp_vision_new_jpeg_context_t;

static esp_vision_new_jpeg_context_t s_new_jpeg;

static size_t esp_vision_new_jpeg_capacity(size_t raw_size)
{
    size_t capacity = raw_size + (raw_size / 2) + 4096;
    return (capacity < 16384) ? 16384 : capacity;
}

static void esp_vision_new_jpeg_free_buffers(void)
{
    if (s_new_jpeg.input_buf != NULL) {
        jpeg_free_align(s_new_jpeg.input_buf);
        s_new_jpeg.input_buf = NULL;
    }
    if (s_new_jpeg.jpeg_buf != NULL) {
        free(s_new_jpeg.jpeg_buf);
        s_new_jpeg.jpeg_buf = NULL;
    }
    s_new_jpeg.input_buf_size = 0;
    s_new_jpeg.jpeg_buf_size = 0;
}

static esp_err_t esp_vision_new_jpeg_ensure_buffers(size_t raw_size)
{
    if (raw_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_new_jpeg.input_buf_size < raw_size) {
        if (s_new_jpeg.input_buf != NULL) {
            jpeg_free_align(s_new_jpeg.input_buf);
            s_new_jpeg.input_buf = NULL;
            s_new_jpeg.input_buf_size = 0;
        }

        s_new_jpeg.input_buf = jpeg_calloc_align(raw_size, 16);
        if (s_new_jpeg.input_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_new_jpeg.input_buf_size = raw_size;
    }

    size_t jpeg_capacity = esp_vision_new_jpeg_capacity(raw_size);
    if (s_new_jpeg.jpeg_buf_size < jpeg_capacity) {
        if (s_new_jpeg.jpeg_buf != NULL) {
            free(s_new_jpeg.jpeg_buf);
            s_new_jpeg.jpeg_buf = NULL;
            s_new_jpeg.jpeg_buf_size = 0;
        }

        s_new_jpeg.jpeg_buf = esp_vision_memory_malloc(jpeg_capacity, MALLOC_CAP_8BIT);
        if (s_new_jpeg.jpeg_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_new_jpeg.jpeg_buf_size = jpeg_capacity;
    }

    return ESP_OK;
}

static esp_err_t esp_vision_new_jpeg_image_format(const image_t *img,
                                                  size_t *raw_size,
                                                  jpeg_pixel_format_t *src_type,
                                                  esp_new_jpeg_subsampling_t *subsampling)
{
    if ((img == NULL) || (img->pixels == NULL) || (img->w <= 0) || (img->h <= 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (img->pixfmt) {
    case PIXFORMAT_GRAYSCALE:
        *raw_size = (size_t)img->w * (size_t)img->h;
        *src_type = JPEG_PIXEL_FORMAT_GRAY;
        *subsampling = JPEG_SUBSAMPLE_GRAY;
        return ESP_OK;
    case PIXFORMAT_RGB565:
        *raw_size = (size_t)img->w * (size_t)img->h * sizeof(uint16_t);
        *src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        *subsampling = JPEG_SUBSAMPLE_420;
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t esp_vision_new_jpeg_encode(const image_t *img, int quality, uint8_t **jpeg_buf, size_t *jpeg_size)
{
    size_t raw_size = 0;
    jpeg_pixel_format_t src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    esp_new_jpeg_subsampling_t subsampling = JPEG_SUBSAMPLE_420;
    esp_err_t ret = esp_vision_new_jpeg_image_format(img, &raw_size, &src_type, &subsampling);
    if (ret != ESP_OK) {
        return ret;
    }
    if ((raw_size > INT_MAX) || (esp_vision_new_jpeg_capacity(raw_size) > INT_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = esp_vision_new_jpeg_ensure_buffers(raw_size);
    if (ret != ESP_OK) {
        return ret;
    }
    memcpy(s_new_jpeg.input_buf, img->pixels, raw_size);

    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = img->w;
    config.height = img->h;
    config.src_type = src_type;
    config.subsampling = subsampling;
    config.quality = (uint8_t)quality;
    config.rotate = JPEG_ROTATE_0D;
    config.task_enable = false;

    jpeg_enc_handle_t encoder = NULL;
    jpeg_error_t jpeg_ret = jpeg_enc_open(&config, &encoder);
    if (jpeg_ret != JPEG_ERR_OK) {
        return esp_vision_jpeg_error_to_esp_err(jpeg_ret);
    }

    int out_size = 0;
    jpeg_ret = jpeg_enc_process(encoder,
                                s_new_jpeg.input_buf,
                                (int)raw_size,
                                s_new_jpeg.jpeg_buf,
                                (int)s_new_jpeg.jpeg_buf_size,
                                &out_size);
    jpeg_enc_close(encoder);
    if (jpeg_ret != JPEG_ERR_OK) {
        return esp_vision_jpeg_error_to_esp_err(jpeg_ret);
    }

    *jpeg_buf = s_new_jpeg.jpeg_buf;
    *jpeg_size = (size_t)out_size;
    return ESP_OK;
}

#endif

#endif

esp_err_t esp_vision_jpeg_decode(const uint8_t *jpeg_buf, size_t jpeg_size, image_t *dst)
{
#if ESP_VISION_HAS_ESP_NEW_JPEG
    if ((jpeg_buf == NULL) || (jpeg_size == 0) || (jpeg_size > INT_MAX) ||
            (dst == NULL) || (dst->pixels == NULL) || (dst->w <= 0) || (dst->h <= 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((dst->pixfmt != PIXFORMAT_RGB565) && (dst->pixfmt != PIXFORMAT_GRAYSCALE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t decoder = NULL;
    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t info = {};
    uint8_t *decode_buf = NULL;

    esp_err_t ret = esp_vision_jpeg_open_decoder(&config, jpeg_buf, jpeg_size, &decoder, &io, &info);
    if (ret != ESP_OK) {
        goto exit;
    }

    bool scaled = ((info.width != (uint16_t)dst->w) || (info.height != (uint16_t)dst->h));
    if ((dst->w > info.width) || (dst->h > info.height) ||
            (scaled && (((dst->w % 8) != 0) || ((dst->h % 8) != 0)))) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    if (scaled) {
        jpeg_dec_close(decoder);
        decoder = NULL;

        config.scale.width = (uint16_t)dst->w;
        config.scale.height = (uint16_t)dst->h;
        ret = esp_vision_jpeg_open_decoder(&config, jpeg_buf, jpeg_size, &decoder, &io, &info);
        if (ret != ESP_OK) {
            goto exit;
        }
    }

    int outbuf_len = 0;
    jpeg_error_t jpeg_ret = jpeg_dec_get_outbuf_len(decoder, &outbuf_len);
    if (jpeg_ret != JPEG_ERR_OK) {
        ret = esp_vision_jpeg_error_to_esp_err(jpeg_ret);
        goto exit;
    }

    size_t pixel_count = (size_t)dst->w * (size_t)dst->h;
    size_t rgb565_size = pixel_count * sizeof(uint16_t);
    if ((outbuf_len < 0) || ((size_t)outbuf_len < rgb565_size)) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    decode_buf = jpeg_calloc_align((size_t)outbuf_len, 16);
    if (decode_buf == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }

    io.outbuf = decode_buf;
    jpeg_ret = jpeg_dec_process(decoder, &io);
    if (jpeg_ret != JPEG_ERR_OK) {
        ret = esp_vision_jpeg_error_to_esp_err(jpeg_ret);
        goto exit;
    }

    if (dst->pixfmt == PIXFORMAT_RGB565) {
        memcpy(dst->pixels, decode_buf, rgb565_size);
    } else {
        esp_vision_jpeg_rgb565_to_grayscale((const uint16_t *)decode_buf, dst->pixels, pixel_count);
    }

exit:
    if (decode_buf != NULL) {
        jpeg_free_align(decode_buf);
    }
    if (decoder != NULL) {
        jpeg_dec_close(decoder);
    }
    return ret;
#else
    (void)jpeg_buf;
    (void)jpeg_size;
    (void)dst;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#if SOC_JPEG_ENCODE_SUPPORTED

#include "driver/jpeg_encode.h"

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
#if ESP_VISION_HAS_NEW_JPEG_ENCODER
    esp_vision_new_jpeg_free_buffers();
#endif
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

#else

void esp_vision_jpeg_deinit(void)
{
#if ESP_VISION_HAS_NEW_JPEG_ENCODER
    esp_vision_new_jpeg_free_buffers();
#endif
}

esp_err_t esp_vision_jpeg_encode(const image_t *img, int quality, uint8_t **jpeg_buf, size_t *jpeg_size)
{
    if ((jpeg_buf == NULL) || (jpeg_size == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

#if ESP_VISION_HAS_NEW_JPEG_ENCODER
    if ((quality < 1) || (quality > 100)) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_vision_new_jpeg_encode(img, quality, jpeg_buf, jpeg_size);
#else
    (void)img;
    (void)quality;

    *jpeg_buf = NULL;
    *jpeg_size = 0;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#endif

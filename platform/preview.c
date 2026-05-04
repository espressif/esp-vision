/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "preview.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "mbedtls/base64.h"
#include "py/mpconfig.h"
#include "py/mphal.h"

#if MICROPY_HW_USB_CDC
#include "shared/tinyusb/mp_usbd_cdc.h"
#endif

#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif

#ifndef NO_QSTR
#include "imlib.h"
#endif

#include "boardconfig.h"
#include "esp_vision_jpeg.h"

#define ESP_VISION_PREVIEW_BASE64_INPUT_CHUNK (768)
#define ESP_VISION_PREVIEW_BASE64_OUTPUT_CHUNK ((ESP_VISION_PREVIEW_BASE64_INPUT_CHUNK / 3) * 4)

static const char *TAG = "esp_vision_preview";

static esp_err_t esp_vision_preview_write(const char *str, size_t len)
{
    if ((str == NULL) || (len == 0)) {
        return ESP_OK;
    }

#if MICROPY_HW_USB_CDC
    mp_uint_t written = mp_usbd_cdc_tx_strn(str, len);
#else
    mp_uint_t written = mp_hal_stdout_tx_strn(str, len);
#endif
    return (written == len) ? ESP_OK : ESP_FAIL;
}

static esp_err_t esp_vision_preview_printf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;

    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len <= 0) {
        return ESP_FAIL;
    }
    if ((size_t)len >= sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return esp_vision_preview_write(buf, (size_t)len);
}

void esp_vision_preview_deinit(void)
{
    esp_vision_jpeg_deinit();
}

void esp_vision_preview_init0(void)
{
    esp_vision_preview_deinit();
}

static esp_err_t esp_vision_preview_write_base64(const uint8_t *data, size_t len)
{
    unsigned char out[ESP_VISION_PREVIEW_BASE64_OUTPUT_CHUNK + 1];
    size_t offset = 0;

    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > ESP_VISION_PREVIEW_BASE64_INPUT_CHUNK) {
            chunk = ESP_VISION_PREVIEW_BASE64_INPUT_CHUNK;
        }

        size_t out_len = 0;
        int ret = mbedtls_base64_encode(out, sizeof(out), &out_len, data + offset, chunk);
        if (ret != 0) {
            return ESP_FAIL;
        }

        ESP_RETURN_ON_ERROR(esp_vision_preview_write((const char *)out, out_len),
                            TAG,
                            "failed to write preview frame");
        offset += chunk;
    }

    return ESP_OK;
}

static esp_err_t esp_vision_preview_write_frame(const image_t *img, const uint8_t *jpeg_buf, size_t jpeg_size)
{
    ESP_RETURN_ON_ERROR(esp_vision_preview_printf("<EVFRAME width=%" PRIi32 " height=%" PRIi32
                                                  " format=jpg size=%u encoding=base64>\r\n",
                                                  img->w,
                                                  img->h,
                                                  (unsigned int)jpeg_size),
                        TAG,
                        "failed to write preview header");

    ESP_RETURN_ON_ERROR(esp_vision_preview_write_base64(jpeg_buf, jpeg_size), TAG, "failed to encode preview frame");
    return esp_vision_preview_write("\r\n</EVFRAME>\r\n", strlen("\r\n</EVFRAME>\r\n"));
}

esp_err_t esp_vision_preview_flush(const image_t *img)
{
    uint8_t *jpeg_buf = NULL;
    size_t jpeg_size = 0;

    ESP_RETURN_ON_ERROR(esp_vision_jpeg_encode(img,
                                               ESP_VISION_JPEG_QUALITY_LOW,
                                               &jpeg_buf,
                                               &jpeg_size),
                        TAG,
                        "failed to encode jpeg");
    return esp_vision_preview_write_frame(img, jpeg_buf, jpeg_size);
}

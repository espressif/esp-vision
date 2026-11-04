/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "preview.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_timer.h"
#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif

#ifndef NO_QSTR
#include "imlib.h"
#endif

#include "boardconfig.h"
#include "ev_channel.h"
#include "ev_mux.h"
#include "jpeg.h"
#include "uvc.h"

static const char *TAG = "esp_vision_preview";
static uint32_t s_preview_seq;
// On the USJ route the preview frame rate is capped: USB Serial JTAG
// throughput is far below OTG CDC, and an app flushing in a tight loop would
// otherwise keep the sink permanently congested (frames dropped anyway) and
// burn CPU on JPEG encodes that never make it onto the wire.
#define ESP_VISION_PREVIEW_USJ_MIN_INTERVAL_MS (100)
static uint32_t s_last_usj_flush_ms;

static bool esp_vision_preview_is_ready(void)
{
    return ev_channel_ready(EV_STREAM_USER);
}

void esp_vision_preview_deinit(void)
{
    esp_vision_jpeg_deinit();
}

void esp_vision_preview_init0(void)
{
    esp_vision_preview_deinit();
    s_preview_seq = 0;
    s_last_usj_flush_ms = 0;
}

static esp_err_t esp_vision_preview_write_frame(const image_t *img, const uint8_t *jpeg_buf, size_t jpeg_size, bool mux_ready, bool observers_ready)
{
    const char *route = ev_channel_get(EV_STREAM_USER);
    uint32_t seq = s_preview_seq++;
    uint32_t ts_ms = (uint32_t)(esp_timer_get_time() / 1000);
    char metadata[224];
    int metadata_len = snprintf(metadata,
                                sizeof(metadata),
                                "{\"sid\":\"preview\",\"seq\":%" PRIu32 ",\"channel\":\"preview.frame\","
                                "\"type\":\"data\",\"encoding\":\"binary\",\"route\":\"%s\","
                                "\"contentType\":\"image/jpeg\",\"ts_ms\":%" PRIu32 ","
                                "\"width\":%" PRIi32 ",\"height\":%" PRIi32 "}",
                                seq,
                                route,
                                ts_ms,
                                img->w,
                                img->h);
    if ((metadata_len <= 0) || ((size_t)metadata_len >= sizeof(metadata))) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (observers_ready) {
        (void)ev_mux_preview_publish_observers(metadata, jpeg_buf, jpeg_size);
    }
    return mux_ready ? ev_mux_write_lossy(EV_STREAM_USER, metadata, jpeg_buf, jpeg_size) : ESP_OK;
}

esp_err_t esp_vision_preview_flush(const image_t *img)
{
    uint8_t *jpeg_buf = NULL;
    size_t jpeg_size = 0;
    esp_err_t ret;

    bool uvc_ready = esp_vision_uvc_preview_ready(img);
    bool mux_ready = esp_vision_preview_is_ready();
    bool observers_ready = ev_mux_preview_observers_ready();
    const char *route = ev_channel_get(EV_STREAM_USER);
    if (mux_ready && strcmp(route, "usj") == 0) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - s_last_usj_flush_ms) < ESP_VISION_PREVIEW_USJ_MIN_INTERVAL_MS) {
            mux_ready = false;
        } else {
            s_last_usj_flush_ms = now_ms;
        }
    }
    if (mux_ready && !ev_mux_preview_available(EV_STREAM_USER)) {
        ev_mux_record_tx_drop();
        mux_ready = false;
    }
    if (!mux_ready && !uvc_ready && !observers_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_vision_jpeg_encode(img,
                                               ESP_VISION_JPEG_QUALITY_LOW,
                                               &jpeg_buf,
                                               &jpeg_size),
                        TAG,
                        "failed to encode jpeg");

    if (uvc_ready) {
        bool accepted;
        // A slow or unavailable UVC subscriber must not affect EV-MUX.
        (void)esp_vision_uvc_publish(jpeg_buf, jpeg_size, false, &accepted);
    }
    if (!mux_ready && !observers_ready) {
        return ESP_OK;
    }
    ret = esp_vision_preview_write_frame(img, jpeg_buf, jpeg_size, mux_ready, observers_ready);
    if ((ret == ESP_ERR_INVALID_STATE) || (ret == ESP_ERR_TIMEOUT) ||
            (ret == ESP_ERR_INVALID_SIZE) || (ret == ESP_ERR_NO_MEM)) {
        return ESP_OK;
    }
    return ret;
}

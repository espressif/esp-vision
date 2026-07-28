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
    ev_channel_init0();
}

static esp_err_t esp_vision_preview_write_frame(const image_t *img, const uint8_t *jpeg_buf, size_t jpeg_size)
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

    return ev_mux_write_lossy(EV_STREAM_USER, metadata, jpeg_buf, jpeg_size);
}

esp_err_t esp_vision_preview_flush(const image_t *img)
{
    uint8_t *jpeg_buf = NULL;
    size_t jpeg_size = 0;
    esp_err_t ret;

    if (!esp_vision_preview_is_ready()) {
        return ESP_OK;
    }
    const char *route = ev_channel_get(EV_STREAM_USER);
    if (strcmp(route, "usj") == 0) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - s_last_usj_flush_ms) < ESP_VISION_PREVIEW_USJ_MIN_INTERVAL_MS) {
            return ESP_OK;
        }
        s_last_usj_flush_ms = now_ms;
    }
    // Skip the encode when the sink is congested: the frame would be dropped
    // whole at write time anyway.
    if (ev_mux_stream_congested(EV_STREAM_USER)) {
        ev_mux_record_tx_drop();
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_vision_jpeg_encode(img,
                                               ESP_VISION_JPEG_QUALITY_LOW,
                                               &jpeg_buf,
                                               &jpeg_size),
                        TAG,
                        "failed to encode jpeg");

    ret = esp_vision_preview_write_frame(img, jpeg_buf, jpeg_size);
    if ((ret == ESP_ERR_INVALID_STATE) || (ret == ESP_ERR_TIMEOUT)) {
        return ESP_OK;
    }
    return ret;
}

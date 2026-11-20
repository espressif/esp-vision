/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "py/mpconfig.h"
#include "uvc.h"

#if MICROPY_HW_ENABLE_USBDEV
#include "shared/tinyusb/mp_usbd.h"
#endif

#if MICROPY_HW_ENABLE_USBDEV && CFG_TUD_VIDEO
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif
#ifndef NO_QSTR
#include "imlib.h"
#endif
#include "jpeg.h"
#include "uvc_logo.h"

#define ESP_VISION_UVC_PREVIEW_HANDOFF_US (40000)

// The USB lock serializes publication, task polling and TinyUSB callbacks.
// A transfer holds its JPEG pointer until a matching completion or a confirmed
// cancellation. Logical source changes can discard a frame before it is safe
// to return its storage.
static uint8_t *s_frame;
static size_t s_frame_capacity;
static size_t s_frame_size;
static uint8_t *s_logo;
static size_t s_logo_size;
static const uint8_t *s_inflight;
static uint32_t s_inflight_epoch;
static uint32_t s_transfer_id;
static uint32_t s_epoch = 1;
static bool s_preview;
static bool s_preview_handoff;
static bool s_stream_started;
static bool s_committed;
static int64_t s_next_frame_us;
static TaskHandle_t s_task;
static esp_vision_uvc_mode_t s_mode;
static esp_vision_uvc_mode_t s_default_mode;
static size_t s_default_frame_limit;
static size_t s_frame_limit;
static esp_vision_uvc_mode_committed_cb_t s_mode_callback;

static void esp_vision_uvc_invalidate_publications(void)
{
    if (++s_epoch == 0) {
        ++s_epoch;
    }
}

static bool esp_vision_uvc_can_stream(void)
{
    return tud_mounted() && !tud_suspended() && tud_video_n_streaming(0, 0);
}

static void esp_vision_uvc_release_unused(void)
{
    if (s_frame_size == 0 && s_frame != s_inflight) {
        free(s_frame);
        s_frame = NULL;
        s_frame_capacity = 0;
    }
    if (s_logo_size == 0 && s_logo != s_inflight) {
        free(s_logo);
        s_logo = NULL;
    }
}

static void esp_vision_uvc_clear_frame(void)
{
    s_frame_size = 0;
    esp_vision_uvc_invalidate_publications();
    esp_vision_uvc_release_unused();
}

void tud_video_stream_closed_cb(uint_fast8_t ctl, uint_fast8_t stm, bool bus_reset)
{
    if (ctl != 0 || stm != 0) {
        return;
    }
    s_stream_started = false;
    s_next_frame_us = 0;
    s_logo_size = 0;
    esp_vision_uvc_clear_frame();
    if (bus_reset) {
        s_mode = s_default_mode;
        s_frame_limit = s_default_frame_limit;
        s_committed = false;
    }
}

void tud_video_frame_xfer_complete_tagged_cb(uint_fast8_t ctl, uint_fast8_t stm,
                                             uint32_t epoch, uint32_t transfer_id)
{
    if (ctl == 0 && stm == 0 && s_inflight != NULL &&
            epoch == s_inflight_epoch && transfer_id == s_transfer_id) {
        s_inflight = NULL;
        esp_vision_uvc_release_unused();
    }
}

void tud_video_frame_xfer_cancelled_cb(uint_fast8_t ctl, uint_fast8_t stm,
                                       uint32_t epoch, uint32_t transfer_id)
{
    tud_video_frame_xfer_complete_tagged_cb(ctl, stm, epoch, transfer_id);
}

static void esp_vision_uvc_poll(void)
{
    if (!tud_mounted() || !tud_video_n_streaming(0, 0)) {
        if (s_stream_started) {
            tud_video_stream_closed_cb(0, 0, !tud_mounted());
        }
        return;
    }
    if (tud_suspended()) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!s_stream_started) {
        // Allow the host one interval to queue its first isochronous receive.
        s_stream_started = true;
        s_next_frame_us = now + s_mode.interval_us;
        return;
    }
    if (s_inflight != NULL || s_preview_handoff || now < s_next_frame_us) {
        return;
    }
    uint8_t *data = s_frame_size != 0 ? s_frame : s_logo;
    size_t size = s_frame_size != 0 ? s_frame_size : s_logo_size;
    if (size == 0) {
        return;
    }
    uint32_t transfer_id = s_transfer_id + 1;
    if (transfer_id == 0) {
        ++transfer_id;
    }
    if (tud_video_n_frame_xfer_tagged(0, 0, data, size, s_epoch, transfer_id)) {
        s_transfer_id = transfer_id;
        s_inflight = data;
        s_inflight_epoch = s_epoch;
        s_next_frame_us = now + s_mode.interval_us;
    }
}

static void esp_vision_uvc_task(void *argument)
{
    (void)argument;
    for (;;) {
        mp_usbd_lock();
        mp_usbd_task();
        esp_vision_uvc_poll();
        uint32_t logo_epoch = 0;
        esp_vision_uvc_mode_t mode = s_mode;
        if (esp_vision_uvc_can_stream() && s_frame_size == 0 && s_logo == NULL &&
                s_inflight == NULL && esp_timer_get_time() >= s_next_frame_us) {
            logo_epoch = s_epoch;
        }
        mp_usbd_unlock();
        if (logo_epoch != 0) {
            size_t size;
            uint8_t *logo = esp_vision_uvc_logo_create(mode.width, mode.height, &size);
            mp_usbd_lock();
            if (s_epoch == logo_epoch && esp_vision_uvc_can_stream() && s_frame_size == 0) {
                if (logo != NULL && size <= s_frame_limit) {
                    s_logo = logo;
                    s_logo_size = size;
                    logo = NULL;
                    esp_vision_uvc_poll();
                } else {
                    s_next_frame_us = esp_timer_get_time() + s_mode.interval_us;
                }
            }
            mp_usbd_unlock();
            free(logo);
        }
        vTaskDelay(pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1);
    }
}

esp_err_t esp_vision_uvc_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    mp_usbd_uvc_mode_t first;
    if (!mp_usbd_uvc_default_mode(&first) ||
            first.width == 0 || first.height == 0 || first.max_frame_size == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_default_mode = (esp_vision_uvc_mode_t) {
        .width = first.width,
        .height = first.height,
        .interval_us = first.interval_100ns / 10,
    };
    s_mode = s_default_mode;
    s_default_frame_limit = first.max_frame_size;
    s_frame_limit = s_default_frame_limit;
    if (xTaskCreate(esp_vision_uvc_task, "uvc", 3072, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int tud_video_commit_cb(uint_fast8_t ctl, uint_fast8_t stm,
                        const video_probe_and_commit_control_t *commit)
{
    mp_usbd_uvc_mode_t selected;
    if (ctl != 0 || stm != 0 || commit->bFormatIndex != 1 ||
            !mp_usbd_uvc_mode_get(commit->bFrameIndex, commit->dwFrameInterval, &selected)) {
        return VIDEO_ERROR_OUT_OF_RANGE;
    }
    s_logo_size = 0;
    s_frame_limit = selected.max_frame_size;
    if (s_frame_size > s_frame_limit) {
        s_frame_size = 0;
    }
    esp_vision_uvc_invalidate_publications();
    esp_vision_uvc_release_unused();
    s_mode = (esp_vision_uvc_mode_t) {
        .width = selected.width,
        .height = selected.height,
        .interval_us = selected.interval_100ns / 10,
    };
    s_committed = true;
    s_stream_started = false;
    s_next_frame_us = 0;
    if (s_mode_callback != NULL) {
        s_mode_callback(&s_mode);
    }
    return VIDEO_ERROR_NONE;
}

void esp_vision_uvc_set_mode_committed_cb(esp_vision_uvc_mode_committed_cb_t callback)
{
    mp_usbd_lock();
    s_mode_callback = callback;
    if (callback != NULL && s_committed) {
        callback(&s_mode);
    }
    mp_usbd_unlock();
}

void esp_vision_uvc_reset(void)
{
    esp_vision_uvc_bind_preview(false);
}

bool esp_vision_uvc_streaming(void)
{
    mp_usbd_lock();
    bool streaming = esp_vision_uvc_can_stream();
    mp_usbd_unlock();
    return streaming;
}

void esp_vision_uvc_get_mode(esp_vision_uvc_mode_t *mode)
{
    mp_usbd_lock();
    *mode = s_mode;
    mp_usbd_unlock();
}

void esp_vision_uvc_bind_preview(bool enabled)
{
    mp_usbd_lock();
    s_preview = enabled;
    esp_vision_uvc_clear_frame();
    mp_usbd_unlock();
}

uint32_t esp_vision_uvc_preview_epoch(void)
{
    mp_usbd_lock();
    // A transfer that is busy now may finish before the preview JPEG is
    // ready. Publication checks that the frame can be replaced before copying.
    uint32_t epoch = (s_preview && esp_vision_uvc_can_stream()) ? s_epoch : 0;
    mp_usbd_unlock();
    return epoch;
}

static esp_err_t esp_vision_uvc_submit(uint32_t epoch, const uint8_t *jpeg, size_t size,
                                       bool from_preview, bool detach_preview, bool *accepted)
{
    *accepted = false;
    const int64_t deadline = from_preview ? esp_timer_get_time() + ESP_VISION_UVC_PREVIEW_HANDOFF_US : 0;
    bool handoff = false;
    mp_usbd_lock();
    esp_err_t result = ESP_OK;
    for (;;) {
        if (epoch != s_epoch || (from_preview && !s_preview) || !esp_vision_uvc_can_stream()) {
            goto done;
        }
        if (jpeg == NULL || size == 0 || size > s_frame_limit) {
            result = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        if (s_inflight == NULL || s_inflight != s_frame) {
            break;
        }
        if (!from_preview || (s_preview_handoff && !handoff) || esp_timer_get_time() >= deadline) {
            goto done;
        }
        // Give an explicitly bound preview a chance to replace the frame
        // when the current transfer releases it, before repeating it again.
        s_preview_handoff = true;
        handoff = true;
        mp_usbd_unlock();
        vTaskDelay(1);
        mp_usbd_lock();
    }
    if (s_frame_capacity < size) {
        // Capacity follows the negotiated profile. realloc preserves the old
        // picture on failure and is only legal when TinyUSB has no reference.
        uint8_t *frame = heap_caps_realloc(s_frame, s_frame_limit, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (frame == NULL) {
            result = ESP_ERR_NO_MEM;
            goto done;
        }
        s_frame = frame;
        s_frame_capacity = s_frame_limit;
    }
    memcpy(s_frame, jpeg, size);
    s_frame_size = size;
    s_logo_size = 0;
    esp_vision_uvc_release_unused();
    if (detach_preview) {
        s_preview = false;
        esp_vision_uvc_invalidate_publications();
    }
    *accepted = true;
done:
    if (handoff) {
        s_preview_handoff = false;
    }
    mp_usbd_unlock();
    return result;
}

esp_err_t esp_vision_uvc_publish(uint32_t epoch, const uint8_t *jpeg, size_t size, bool *accepted)
{
    return esp_vision_uvc_submit(epoch, jpeg, size, true, false, accepted);
}

esp_err_t esp_vision_uvc_write(const image_t *img, bool detach_preview, bool *accepted)
{
    *accepted = false;
    mp_usbd_lock();
    uint32_t epoch = esp_vision_uvc_can_stream() ? s_epoch : 0;
    bool application_frame_busy = s_inflight != NULL && s_inflight == s_frame;
    mp_usbd_unlock();
    // A JPEG input is checked for size by the cheap passthrough path even
    // when busy. Avoid running the encoder for a raw frame we cannot accept.
    if (epoch == 0 || (application_frame_busy && img != NULL && img->pixfmt != PIXFORMAT_JPEG)) {
        return ESP_OK;
    }
    // Encode on the VM task and copy before returning. The USB task only
    // references the independent UVC buffer or the generated logo.
    uint8_t *jpeg = NULL;
    size_t size = 0;
    esp_err_t result = esp_vision_jpeg_encode(img, 50, &jpeg, &size);
    if (result == ESP_OK) {
        result = esp_vision_uvc_submit(epoch, jpeg, size, false, detach_preview, accepted);
    }
    return result;
}
#else
esp_err_t esp_vision_uvc_init(void)
{
    return ESP_OK;
}

void esp_vision_uvc_reset(void) { }

bool esp_vision_uvc_streaming(void)
{
    return false;
}

void esp_vision_uvc_get_mode(esp_vision_uvc_mode_t *mode)
{
    *mode = (esp_vision_uvc_mode_t) {
        0
    };
}

void esp_vision_uvc_set_mode_committed_cb(esp_vision_uvc_mode_committed_cb_t callback)
{
    (void)callback;
}

void esp_vision_uvc_bind_preview(bool enabled)
{
    (void)enabled;
}

uint32_t esp_vision_uvc_preview_epoch(void)
{
    return 0;
}

esp_err_t esp_vision_uvc_publish(uint32_t epoch, const uint8_t *jpeg, size_t size, bool *accepted)
{
    (void)epoch;
    (void)jpeg;
    (void)size;
    *accepted = false;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_vision_uvc_write(const image_t *img, bool detach_preview, bool *accepted)
{
    (void)img;
    (void)detach_preview;
    *accepted = false;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

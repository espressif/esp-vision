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
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif
#ifndef NO_QSTR
#include "imlib.h"
#endif
#include "jpeg.h"
#include "device/usbd_pvt.h"
#include "uvc_logo.h"

// TinyUSB copies source bytes to its aligned internal endpoint buffer before
// DMA. These two persistent PSRAM slots are never passed directly to DMA and
// never freed across a soft reset. All slot/source state is under the USB lock.
static uint8_t *s_frames[2];
static size_t s_sizes[2];
static int s_latest = -1;
static int s_inflight = -1;
static bool s_busy;
static bool s_preview;
static bool s_stream_started;
static int64_t s_next_frame_us;
static TaskHandle_t s_task;

static void esp_vision_uvc_poll(void)
{
    if (!tud_mounted() || !tud_video_n_streaming(0, 0)) {
        // Video alternate setting zero / bus reset clears TinyUSB's source
        // pointer before this poll. Its endpoint buffer owns any remaining DMA.
        s_busy = false;
        s_inflight = -1;
        s_stream_started = false;
        s_next_frame_us = 0;
        return;
    }
    if (tud_suspended()) {
        return;
    }
    // SET_INTERFACE can cancel a transfer without the completion callback.
    // Retain the old slot until the class accepts a new source pointer.
    if (s_busy && !usbd_edpt_busy(TUD_OPT_RHPORT, USBD_UVC_EP_IN)) {
        s_busy = false;
    }
    int64_t now = esp_timer_get_time();
    if (!s_stream_started) {
        // SET_INTERFACE activates the endpoint before the host has submitted
        // its first ISO receive URBs. Give it one frame interval to queue them
        // so the first delivered JPEG includes its SOI/header rather than a tail.
        s_stream_started = true;
        s_next_frame_us = now + ESP_VISION_UVC_INTERVAL_US;
        return;
    }
    if (s_busy || now < s_next_frame_us) {
        return;
    }
    int slot = s_latest;
    const uint8_t *data = slot < 0 ? esp_vision_uvc_logo : s_frames[slot];
    size_t size = slot < 0 ? sizeof(esp_vision_uvc_logo) : s_sizes[slot];
    if (tud_video_n_frame_xfer(0, 0, (void *)data, size)) {
        s_inflight = slot;
        s_busy = true;
        s_next_frame_us = now + ESP_VISION_UVC_INTERVAL_US;
    }
}

static void esp_vision_uvc_task(void *arg)
{
    (void)arg;
    for (;;) {
        mp_usbd_lock();
        mp_usbd_task();
        esp_vision_uvc_poll();
        mp_usbd_unlock();
        vTaskDelay(pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1);
    }
}

esp_err_t esp_vision_uvc_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreate(esp_vision_uvc_task, "uvc", 3072, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx)
{
    (void)ctl_idx;
    (void)stm_idx;
    s_busy = false;
    s_inflight = -1;
}

// The scoped TinyUSB class reports aborted/error transfers separately from a
// complete frame. Both release the immutable slot; neither retries old data.
void tud_video_frame_xfer_failed_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx)
{
    (void)ctl_idx;
    (void)stm_idx;
    s_busy = false;
    s_inflight = -1;
}

int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx,
                        const video_probe_and_commit_control_t *mode)
{
    if (ctl_idx != 0 || stm_idx != 0 || mode->bFormatIndex != 1 ||
            mode->bFrameIndex != 1 || mode->dwFrameInterval != ESP_VISION_UVC_INTERVAL_US * 10) {
        return VIDEO_ERROR_OUT_OF_RANGE;
    }
    // A successful commit resets the video class's source pointer. Old DMA
    // references only its private endpoint buffer, never these source slots.
    s_busy = false;
    s_inflight = -1;
    s_stream_started = false;
    s_next_frame_us = 0;
    return VIDEO_ERROR_NONE;
}

void esp_vision_uvc_reset(void)
{
    mp_usbd_lock();
    s_latest = -1;
    s_preview = false;
    // Keep an in-flight slot reserved until completion/cancellation.
    mp_usbd_unlock();
}

bool esp_vision_uvc_streaming(void)
{
    mp_usbd_lock();
    bool ready = tud_mounted() && !tud_suspended() && tud_video_n_streaming(0, 0);
    mp_usbd_unlock();
    return ready;
}

void esp_vision_uvc_bind_preview(bool enabled)
{
    mp_usbd_lock();
    s_preview = enabled;
    s_latest = -1;
    mp_usbd_unlock();
}

bool esp_vision_uvc_preview_ready(const image_t *img)
{
    mp_usbd_lock();
    bool ready = s_preview && img->w == ESP_VISION_UVC_WIDTH &&
                 img->h == ESP_VISION_UVC_HEIGHT && tud_mounted() &&
                 !tud_suspended() && tud_video_n_streaming(0, 0);
    mp_usbd_unlock();
    return ready;
}

esp_err_t esp_vision_uvc_publish(const uint8_t *jpeg, size_t size, bool fixed, bool *accepted)
{
    *accepted = false;
    if (size == 0 || size > ESP_VISION_UVC_MAX_FRAME) {
        return ESP_ERR_INVALID_SIZE;
    }
    mp_usbd_lock();
    if (!fixed && (!tud_mounted() || tud_suspended() || !tud_video_n_streaming(0, 0))) {
        mp_usbd_unlock();
        return ESP_OK;
    }
    int slot = s_inflight == 0 ? 1 : 0;
    if (s_frames[slot] == NULL) {
        s_frames[slot] = heap_caps_malloc(ESP_VISION_UVC_MAX_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_frames[slot] == NULL) {
            mp_usbd_unlock();
            return ESP_ERR_NO_MEM;
        }
    }
    memcpy(s_frames[slot], jpeg, size);
    s_sizes[slot] = size;
    s_latest = slot;
    if (fixed) {
        s_preview = false;
    }
    *accepted = true;
    mp_usbd_unlock();
    return ESP_OK;
}

esp_err_t esp_vision_uvc_write(const image_t *img, bool fixed, bool *accepted)
{
    *accepted = false;
    if (img->w != ESP_VISION_UVC_WIDTH || img->h != ESP_VISION_UVC_HEIGHT) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!fixed && !esp_vision_uvc_streaming()) {
        return ESP_OK;
    }
    uint8_t *jpeg;
    size_t size;
    esp_err_t err = esp_vision_jpeg_encode(img, 50, &jpeg, &size);
    return err == ESP_OK ? esp_vision_uvc_publish(jpeg, size, fixed, accepted) : err;
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
void esp_vision_uvc_bind_preview(bool enabled)
{
    (void)enabled;
}
bool esp_vision_uvc_preview_ready(const image_t *img)
{
    (void)img;
    return false;
}
esp_err_t esp_vision_uvc_publish(const uint8_t *jpeg, size_t size, bool fixed, bool *accepted)
{
    (void)jpeg; (void)size; (void)fixed;
    *accepted = false;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_vision_uvc_write(const image_t *img, bool fixed, bool *accepted)
{
    (void)img; (void)fixed;
    *accepted = false;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

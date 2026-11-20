/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "imlib.h"
#include "jpeg.h"
#include "shared/tinyusb/mp_usbd.h"
#include "uvc.h"
#include "uvc_logo.h"

int tud_video_commit_cb(uint_fast8_t ctl, uint_fast8_t stm,
                        const video_probe_and_commit_control_t *mode);

static bool s_mounted = true;
static bool s_suspended;
static bool s_streaming;
static bool s_usb_locked;
static bool s_close_during_encode;
static bool s_alloc_fail;
static bool s_waiting_to_publish;
static bool s_finish_during_wait;
static unsigned s_encodes;
static unsigned s_allocations;
static unsigned s_frame_frees;
static unsigned s_logos;
static unsigned s_submissions;
static unsigned s_mode_notifications;
static int s_encoded_width;
static void *s_frame;
static const uint8_t *s_usb_source;
static size_t s_usb_size;
static uint32_t s_usb_epoch;
static uint32_t s_usb_transfer;
static size_t s_encoded_size = 4;
static uint8_t s_encoded[48 * 1024 + 1] = {0xff, 0xd8, 0xff, 0xd9};
static int64_t s_now;
static TaskFunction_t s_task;
static jmp_buf s_task_yield;

static void finish(bool cancelled);
static void poll_at(int64_t now);

void __real_free(void *ptr);

void __wrap_free(void *ptr)
{
    assert(ptr == NULL || ptr != s_usb_source);
    if (ptr != NULL && ptr == s_frame) {
        ++s_frame_frees;
        s_frame = NULL;
    }
    __real_free(ptr);
}

void *heap_caps_realloc(void *ptr, size_t size, int capabilities)
{
    assert(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(ptr == s_frame);
    assert(ptr == NULL || ptr != s_usb_source);
    if (s_alloc_fail) {
        return NULL;
    }
    s_frame = realloc(ptr, size);
    assert(s_frame != NULL);
    ++s_allocations;
    return s_frame;
}

uint8_t *esp_vision_uvc_logo_create(uint16_t width, uint16_t height, size_t *size)
{
    assert(!s_usb_locked);
    uint8_t *logo = malloc(4);
    assert(logo != NULL);
    logo[0] = width >> 8;
    logo[1] = width;
    logo[2] = height >> 8;
    logo[3] = height;
    *size = 4;
    ++s_logos;
    return logo;
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_size,
                       void *argument, unsigned priority, TaskHandle_t *handle)
{
    (void)name;
    (void)stack_size;
    (void)argument;
    (void)priority;
    s_task = task;
    *handle = (TaskHandle_t)(uintptr_t)1;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0 && !s_usb_locked);
    if (s_waiting_to_publish) {
        s_now += (int64_t)ticks * 1000;
        if (s_finish_during_wait) {
            s_finish_during_wait = false;
            finish(false);
            // Running the USB poll before the writer resumes must not repeat
            // the outgoing frame and claim the slot a second time.
            unsigned submissions = s_submissions;
            s_waiting_to_publish = false;
            poll_at(s_now);
            s_waiting_to_publish = true;
            assert(s_submissions == submissions);
        }
        return;
    }
    longjmp(s_task_yield, 1);
}

void mp_usbd_lock(void)
{
    assert(!s_usb_locked);
    s_usb_locked = true;
}

void mp_usbd_unlock(void)
{
    assert(s_usb_locked);
    s_usb_locked = false;
}

void mp_usbd_task(void)
{
    assert(s_usb_locked);
}

bool mp_usbd_uvc_mode_get(uint8_t index, uint32_t interval_100ns, mp_usbd_uvc_mode_t *mode)
{
    if (index == 1 && interval_100ns == 1000000) {
        *mode = (mp_usbd_uvc_mode_t) {
            320, 240, 1000000, 49152
        };
        return true;
    }
    if (index == 2 && interval_100ns == 500000) {
        *mode = (mp_usbd_uvc_mode_t) {
            640, 480, 500000, 65536
        };
        return true;
    }
    return false;
}

bool mp_usbd_uvc_default_mode(mp_usbd_uvc_mode_t *mode)
{
    return mp_usbd_uvc_mode_get(1, 1000000, mode);
}

bool tud_mounted(void)
{
    return s_mounted;
}

bool tud_suspended(void)
{
    return s_suspended;
}

bool tud_video_n_streaming(uint_fast8_t ctl, uint_fast8_t stm)
{
    assert(ctl == 0 && stm == 0);
    return s_streaming;
}

bool tud_video_n_frame_xfer_tagged(uint_fast8_t ctl, uint_fast8_t stm, void *buffer,
                                   size_t length, uint32_t epoch, uint32_t transfer_id)
{
    assert(s_usb_locked && s_streaming && s_usb_source == NULL);
    assert(ctl == 0 && stm == 0);
    s_usb_source = buffer;
    s_usb_size = length;
    s_usb_epoch = epoch;
    s_usb_transfer = transfer_id;
    ++s_submissions;
    return true;
}

int64_t esp_timer_get_time(void)
{
    return s_now;
}

esp_err_t esp_vision_jpeg_encode(const image_t *image, int quality,
                                 uint8_t **data, size_t *size)
{
    assert(!s_usb_locked && image != NULL && quality == 50);
    ++s_encodes;
    s_encoded_width = image->w;
    if (s_close_during_encode) {
        mp_usbd_lock();
        tud_video_stream_closed_cb(0, 0, false);
        mp_usbd_unlock();
        s_close_during_encode = false;
    }
    *data = s_encoded;
    *size = s_encoded_size;
    return ESP_OK;
}

static void poll_at(int64_t now)
{
    s_now = now;
    if (setjmp(s_task_yield) == 0) {
        s_task(NULL);
    }
}

static void finish(bool cancelled)
{
    assert(s_usb_source != NULL);
    s_usb_source = NULL;
    mp_usbd_lock();
    if (cancelled) {
        tud_video_frame_xfer_cancelled_cb(0, 0, s_usb_epoch, s_usb_transfer);
    } else {
        tud_video_frame_xfer_complete_tagged_cb(0, 0, s_usb_epoch, s_usb_transfer);
    }
    mp_usbd_unlock();
}

static void close_stream(bool bus_reset)
{
    s_streaming = false;
    mp_usbd_lock();
    tud_video_stream_closed_cb(0, 0, bus_reset);
    mp_usbd_unlock();
}

static void mode_notified(const esp_vision_uvc_mode_t *mode)
{
    assert(s_usb_locked);
    assert(mode->width == 640 && mode->height == 480 && mode->interval_us == 50000);
    ++s_mode_notifications;
}

static bool write_frame(bool detach)
{
    const image_t image = {.w = 160, .h = 120};
    bool accepted;
    assert(esp_vision_uvc_write(&image, detach, &accepted) == ESP_OK);
    return accepted;
}

static void test_display_and_lifetime(void)
{
    assert(esp_vision_uvc_init() == ESP_OK);
    assert(!esp_vision_uvc_streaming());
    assert(!write_frame(false) && !write_frame(true));
    assert(s_encodes == 0 && s_allocations == 0);
    poll_at(1000000);
    assert(s_logos == 0 && s_submissions == 0);

    s_streaming = true;
    poll_at(1100000);
    poll_at(1200000);
    assert(s_logos == 1 && s_submissions == 1 && s_allocations == 0);
    assert(s_usb_source[0] == 1 && s_usb_source[1] == 64);

    esp_vision_uvc_bind_preview(true);
    const uint32_t preview_epoch = esp_vision_uvc_preview_epoch();
    assert(preview_epoch != 0);
    assert(write_frame(false));
    assert(esp_vision_uvc_preview_epoch() == preview_epoch);
    assert(s_encoded_width == 160 && s_allocations == 1);
    finish(false);
    poll_at(1300000);
    assert(s_usb_source == s_frame && s_usb_size == 4);
    assert(s_usb_source[0] == 0xff);
    s_encoded[0] = 0;
    assert(s_usb_source[0] == 0xff);
    assert(!write_frame(true));
    assert(esp_vision_uvc_preview_epoch() == preview_epoch);
    bool accepted;
    s_waiting_to_publish = true;
    assert(esp_vision_uvc_publish(preview_epoch, s_encoded, 4, &accepted) == ESP_OK);
    assert(!accepted);
    assert(s_usb_source[0] == 0xff && s_allocations == 1);
    uint32_t old_epoch = s_usb_epoch;
    uint32_t old_transfer = s_usb_transfer;
    s_finish_during_wait = true;
    // The previous transfer already used up one nominal frame period.
    s_now = 1400000;
    assert(esp_vision_uvc_publish(preview_epoch, s_encoded, 4, &accepted) == ESP_OK);
    assert(accepted && s_usb_source == NULL && s_allocations == 1);
    s_waiting_to_publish = false;
    assert(esp_vision_uvc_preview_epoch() == preview_epoch);
    poll_at(1410000);
    assert(s_usb_source == s_frame && s_usb_source[0] == 0);
    finish(false);
    assert(write_frame(true));
    assert(esp_vision_uvc_preview_epoch() == 0);
    poll_at(1510000);
    assert(s_usb_source == s_frame && s_usb_source[0] == 0);

    close_stream(false);
    assert(s_frame_frees == 0);
    mp_usbd_lock();
    tud_video_frame_xfer_cancelled_cb(0, 0, old_epoch, old_transfer);
    mp_usbd_unlock();
    assert(s_frame_frees == 0);
    s_streaming = true;
    poll_at(1600000);
    poll_at(1700000);
    assert(s_usb_source == s_frame && s_submissions == 4);
    finish(true);
    assert(s_frame == NULL && s_frame_frees == 1);
    poll_at(1800000);
    assert(s_logos == 2 && s_submissions == 5);
    finish(false);
    close_stream(true);
}

static void test_mode_and_publication_epoch(void)
{
    assert(esp_vision_uvc_init() == ESP_OK);
    esp_vision_uvc_mode_t mode;
    esp_vision_uvc_get_mode(&mode);
    assert(mode.width == 320 && mode.height == 240 && mode.interval_us == 100000);
    esp_vision_uvc_set_mode_committed_cb(mode_notified);
    assert(s_mode_notifications == 0);
    video_probe_and_commit_control_t requested = {1, 2, 333333};
    mp_usbd_lock();
    assert(tud_video_commit_cb(0, 0, &requested) == VIDEO_ERROR_OUT_OF_RANGE);
    requested.dwFrameInterval = 500000;
    assert(tud_video_commit_cb(0, 0, &requested) == VIDEO_ERROR_NONE);
    mp_usbd_unlock();
    assert(s_mode_notifications == 1);
    esp_vision_uvc_set_mode_committed_cb(NULL);
    esp_vision_uvc_set_mode_committed_cb(mode_notified);
    assert(s_mode_notifications == 2);
    esp_vision_uvc_reset();
    mp_usbd_lock();
    assert(tud_video_commit_cb(0, 0, &requested) == VIDEO_ERROR_NONE);
    mp_usbd_unlock();
    assert(s_mode_notifications == 3);

    s_streaming = true;
    esp_vision_uvc_bind_preview(true);
    uint32_t old = esp_vision_uvc_preview_epoch();
    assert(old != 0);
    close_stream(false);
    s_streaming = true;
    uint32_t current = esp_vision_uvc_preview_epoch();
    assert(current != 0 && current != old);
    bool accepted = true;
    assert(esp_vision_uvc_publish(old, s_encoded, 4, &accepted) == ESP_OK && !accepted);
    s_close_during_encode = true;
    assert(!write_frame(false));
    assert(s_encodes == 1 && s_allocations == 0);

    s_encoded_size = sizeof(s_encoded);
    assert(write_frame(false)); // fits the committed 64 KiB mode
    assert(s_allocations == 1);
    s_alloc_fail = true;
    s_encoded_size = 4;
    assert(write_frame(false)); // existing slot needs no allocation
    close_stream(true);
    s_streaming = true;
    const image_t image = {.w = 320, .h = 240};
    s_encoded_size = sizeof(s_encoded);
    assert(esp_vision_uvc_write(&image, true, &accepted) == ESP_ERR_INVALID_SIZE && !accepted);
    assert(esp_vision_uvc_preview_epoch() != 0);
    s_encoded_size = 4;
    assert(esp_vision_uvc_write(&image, true, &accepted) == ESP_ERR_NO_MEM && !accepted);
    assert(esp_vision_uvc_preview_epoch() != 0);
    s_suspended = true;
    assert(!esp_vision_uvc_streaming());
    assert(!write_frame(true));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "frame-lifetime") == 0) {
        test_display_and_lifetime();
    } else {
        assert(strcmp(argv[1], "modes-and-epochs") == 0);
        test_mode_and_publication_epoch();
    }
    puts(argv[1]);
    return 0;
}

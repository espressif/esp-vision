/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t interval_100ns;
    uint32_t max_frame_size;
} mp_usbd_uvc_mode_t;

typedef struct {
    uint8_t bFormatIndex;
    uint8_t bFrameIndex;
    uint32_t dwFrameInterval;
} video_probe_and_commit_control_t;

enum {
    VIDEO_ERROR_NONE = 0,
    VIDEO_ERROR_OUT_OF_RANGE = 1,
};

void mp_usbd_lock(void);
void mp_usbd_unlock(void);
void mp_usbd_task(void);
bool mp_usbd_uvc_default_mode(mp_usbd_uvc_mode_t *mode_out);
bool mp_usbd_uvc_mode_get(uint8_t frame_index, uint32_t interval_100ns,
                          mp_usbd_uvc_mode_t *mode_out);

bool tud_mounted(void);
bool tud_suspended(void);
bool tud_video_n_streaming(uint_fast8_t ctl_index, uint_fast8_t stream_index);
bool tud_video_n_frame_xfer_tagged(uint_fast8_t ctl_index, uint_fast8_t stream_index,
                                   void *buffer, size_t buffer_size, uint32_t epoch,
                                   uint32_t transfer_id);
void tud_video_frame_xfer_complete_tagged_cb(uint_fast8_t ctl_index, uint_fast8_t stream_index,
                                             uint32_t epoch, uint32_t transfer_id);
void tud_video_frame_xfer_cancelled_cb(uint_fast8_t ctl_index, uint_fast8_t stream_index,
                                       uint32_t epoch, uint32_t transfer_id);
void tud_video_stream_closed_cb(uint_fast8_t ctl_index, uint_fast8_t stream_index, bool bus_reset);

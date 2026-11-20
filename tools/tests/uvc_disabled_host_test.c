/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>

#include "uvc.h"

int main(void)
{
    esp_vision_uvc_mode_t mode = {
        .width = 1,
        .height = 1,
        .interval_us = 1,
    };
    bool accepted = true;

    assert(esp_vision_uvc_init() == ESP_OK);
    assert(!esp_vision_uvc_streaming());
    esp_vision_uvc_get_mode(&mode);
    assert(mode.width == 0 && mode.height == 0 && mode.interval_us == 0);
    assert(esp_vision_uvc_preview_epoch() == 0);
    assert(esp_vision_uvc_publish(0, NULL, 0, &accepted) == ESP_ERR_NOT_SUPPORTED);
    esp_vision_uvc_set_mode_committed_cb(NULL);
    assert(!accepted);
    assert(esp_vision_uvc_write(NULL, false, &accepted) == ESP_ERR_NOT_SUPPORTED);
    assert(!accepted);
    return 0;
}

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera.h"

// Every board links this fallback; boards with fallible shutdown override it.
__attribute__((weak)) esp_err_t esp_vision_camera_shutdown(void)
{
    if (!esp_vision_camera_is_vm_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_vision_camera_deinit();
    return ESP_OK;
}

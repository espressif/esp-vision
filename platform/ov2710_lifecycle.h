/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_cam_sensor.h"

// Quiesce and delete native statistics callbacks while SCCB is still alive.
// Failed preparation retains the sensor and its callback context for retry.
esp_err_t esp_vision_ov2710_prepare_delete(esp_cam_sensor_device_t *dev);

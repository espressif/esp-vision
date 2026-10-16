/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>

#define ESP_VISION_HOSTNAME_MAX (63)
#define ESP_VISION_DEVICE_ID_LENGTH (12)

/* Initialized before network/task startup; immutable across transports and VM resets. */
bool esp_vision_identity_init(void);
const char *esp_vision_device_id(void);
const char *esp_vision_hostname(void);

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t esp_vision_usb_auto_download_init(void);
void esp_vision_usb_auto_download_line_state(bool dtr, bool rts);

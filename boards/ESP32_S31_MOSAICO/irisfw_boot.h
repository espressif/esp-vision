/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Accept the retained application after MicroPython and EV-MUX are ready. */
esp_err_t esp_vision_irisfw_confirm_boot(void);

/* Published after OTA acceptance and the retained metadata commit succeed. */
bool esp_vision_irisfw_boot_confirmed(void);

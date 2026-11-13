/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include "esp_err.h"
esp_err_t esp_flash_read(void *, void *, uint32_t, uint32_t);

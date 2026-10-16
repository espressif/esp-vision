/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

typedef void (*shutdown_handler_t)(void);

esp_err_t esp_register_shutdown_handler(shutdown_handler_t handler);
void esp_restart(void);

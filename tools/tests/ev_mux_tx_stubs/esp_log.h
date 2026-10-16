/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdarg.h>

typedef int (*vprintf_like_t)(const char *format, va_list args);

vprintf_like_t esp_log_set_vprintf(vprintf_like_t function);

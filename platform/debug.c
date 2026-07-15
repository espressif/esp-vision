/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "debug.h"

#include <stdio.h>

#include "py/mpconfig.h"
#include "ev_channel.h"
#include "ev_stdio.h"

void esp_vision_debug_write(const char *str, size_t len)
{
    if ((str == NULL) || (len == 0)) {
        return;
    }

    if (!ev_stdio_write_idf_log(str, len)) {
        ev_channel_write(EV_STREAM_DEBUG, str, len);
    }
}

void esp_vision_debug_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (len <= 0) {
        return;
    }

    size_t write_len = (size_t)len;
    if (write_len >= sizeof(buf)) {
        write_len = sizeof(buf) - 1;
    }

    esp_vision_debug_write(buf, write_len);
}

void esp_vision_debug_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    esp_vision_debug_vprintf(fmt, ap);
    va_end(ap);
}

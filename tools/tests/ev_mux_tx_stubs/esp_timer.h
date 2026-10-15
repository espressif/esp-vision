/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

extern int64_t ev_mux_test_time_us;

static inline int64_t esp_timer_get_time(void)
{
    return ev_mux_test_time_us;
}

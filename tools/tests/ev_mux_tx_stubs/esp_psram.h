/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

extern bool ev_mux_test_psram_initialized;

static inline bool esp_psram_is_initialized(void)
{
    return ev_mux_test_psram_initialized;
}

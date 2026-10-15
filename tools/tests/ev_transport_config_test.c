/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EV_CONFIG_TEST_DEFAULT_SINK
#define EV_CONFIG_TEST_DEFAULT_SINK "usj"
#endif
#ifndef EV_CONFIG_TEST_FALLBACK_SINK
#define EV_CONFIG_TEST_FALLBACK_SINK EV_CONFIG_TEST_DEFAULT_SINK
#endif
#ifndef EV_CONFIG_TEST_EXPECT_UART_REPL
#error "Test case must state the effective UART REPL setting"
#endif

#define ESP_VISION_TRANSPORT_DEFAULT_SINK EV_CONFIG_TEST_DEFAULT_SINK
#define ESP_VISION_TRANSPORT_FALLBACK_SINK EV_CONFIG_TEST_FALLBACK_SINK

#include <stddef.h>

#include "ev_transport_config.h"

#if MICROPY_HW_ENABLE_UART_REPL != EV_CONFIG_TEST_EXPECT_UART_REPL
#error "transport config must use the effective ESP32 UART REPL default"
#endif

int main(void)
{
    return 0;
}

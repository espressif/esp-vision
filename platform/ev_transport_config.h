/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "py/mpconfig.h"
#include "uart.h"
#include "boardconfig.h"

#ifndef ESP_VISION_TRANSPORT_DEFAULT_SINK
#error "Board must explicitly define ESP_VISION_TRANSPORT_DEFAULT_SINK"
#endif
#ifndef ESP_VISION_TRANSPORT_FALLBACK_SINK
#error "Board must explicitly define ESP_VISION_TRANSPORT_FALLBACK_SINK"
#endif

// The GCC builtin evaluates literal board IDs during compilation. A default
// must name an enabled physical serial backend; TCP requires provisioning.
#if defined(MICROPY_HW_ESP_USB_SERIAL_JTAG)
#define EV_TRANSPORT_CONFIG_USJ_ENABLED (MICROPY_HW_ESP_USB_SERIAL_JTAG)
#else
#define EV_TRANSPORT_CONFIG_USJ_ENABLED (0)
#endif

#if defined(MICROPY_HW_USB_CDC)
#define EV_TRANSPORT_CONFIG_USB_ENABLED (MICROPY_HW_USB_CDC)
#else
#define EV_TRANSPORT_CONFIG_USB_ENABLED (0)
#endif

#define EV_TRANSPORT_CONFIG_SERIAL_SUPPORTED(name) \
    (((__builtin_strcmp((name), "usj") == 0) && EV_TRANSPORT_CONFIG_USJ_ENABLED) || \
     ((__builtin_strcmp((name), "usb") == 0) && EV_TRANSPORT_CONFIG_USB_ENABLED) || \
     ((__builtin_strcmp((name), "uart") == 0) && MICROPY_HW_ENABLE_UART_REPL))

_Static_assert(EV_TRANSPORT_CONFIG_SERIAL_SUPPORTED(ESP_VISION_TRANSPORT_DEFAULT_SINK),
               "EV-MUX default must name an enabled physical serial endpoint");
_Static_assert(EV_TRANSPORT_CONFIG_SERIAL_SUPPORTED(ESP_VISION_TRANSPORT_FALLBACK_SINK),
               "EV-MUX fallback must name an enabled physical serial endpoint");

#undef EV_TRANSPORT_CONFIG_SERIAL_SUPPORTED
#undef EV_TRANSPORT_CONFIG_USB_ENABLED
#undef EV_TRANSPORT_CONFIG_USJ_ENABLED

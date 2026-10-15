/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* Mirror the relevant ESP32 port defaults after mpconfigboard.h. */
#ifndef MICROPY_HW_ENABLE_USBDEV
#define MICROPY_HW_ENABLE_USBDEV (0)
#endif
#ifndef MICROPY_HW_USB_CDC
#define MICROPY_HW_USB_CDC (MICROPY_HW_ENABLE_USBDEV)
#endif
#ifndef MICROPY_HW_ESP_USB_SERIAL_JTAG
#define MICROPY_HW_ESP_USB_SERIAL_JTAG (0)
#endif

/* MICROPY_HW_ENABLE_UART_REPL intentionally has no port default. */

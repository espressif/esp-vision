// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Internal compatibility names; both hosts execute the same ROM engine.
export { createSerialLoader as createBrowserLoader, createOfficialSerialLoader as createOfficialBrowserLoader,
  serialRomInterface as browserRomInterface, ROM_UART_BAUD_RATES } from './rom_loader.js'

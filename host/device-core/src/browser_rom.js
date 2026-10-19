// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Loaded only by openBrowserRom.initialize(), never by the root/Node entry point.
import { connectWithPort } from 'tasmota-webserial-esptool'
import { browserRomCompatibility } from './browser_rom_compat.js'

export async function createBrowserLoader(port, logger) {
  return browserRomCompatibility(await connectWithPort(port, logger))
}

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Node-only adapters. Creating them does not acquire a session or serial port.
export { tcpTransport, serialTransport } from './node_transport.js'
export { openNodeRom } from './node_rom.js'
export { defaultPython, filterSerialPorts, inventory, listSerial, serialEntry, transportFor } from './node_inventory.js'

// Bundlers copy these exact published adapters beside their bundled Node entry.
// Returning URLs is passive and keeps transport assets owned by the SDK.
export function nodeAdapterAssets() {
  return Object.freeze({
    serial: new URL('./node_serial.py', import.meta.url),
    rom: new URL('./node_rom.py', import.meta.url),
  })
}

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

// This option is validated before opening a port or loading a ROM backend.
export function normalizeExpectedMac(value) {
  if (value === undefined) return undefined
  if (typeof value !== 'string' || value.length !== 12 || !/^[0-9a-f]{12}$/i.test(value)) throw new TypeError('expectedMac must contain exactly 12 hexadecimal characters')
  return value.toLowerCase()
}

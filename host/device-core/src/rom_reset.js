// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

// P4/S31 software download requests survive a CPU reset. Preserve every other
// live register field and verify the change before requesting normal SPI boot.
export async function clearDownloadLatch(chipFamily, registers) {
  if (chipFamily !== 0x3280 && chipFamily !== 0x3231) return
  const address = chipFamily === 0x3280 ? 0x50110008 : 0x20700008, mask = 4
  const before = await registers.readRegister(address)
  if (!Number.isInteger(before) || before < 0 || before > 0xffffffff) throw new Error('Invalid download system control register')
  if (before & mask) await registers.writeRegister(address, 0, mask)
  const after = await registers.readRegister(address)
  if (after !== ((before & ~mask) >>> 0)) throw new Error('Normal boot latch verification failed')
}

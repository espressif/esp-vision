// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { createHash } from 'node:crypto'
import { createDevice } from '../src/index.js'
import { firmwareSha256 } from '../src/firmware_flash.js'

export function partitionTable(entries = [
  { label: 'nvs', type: 1, subtype: 2, offset: 0x9000, size: 0x6000 },
  { label: 'phy_init', type: 1, subtype: 1, offset: 0xf000, size: 0x1000 },
  { label: 'factory', type: 0, subtype: 0, offset: 0x10000, size: 0x10000 },
  { label: 'ffat', type: 1, subtype: 0x81, offset: 0x20000, size: 0x21000 },
], { md5 = true, size = 0xc00 } = {}) {
  const bytes = new Uint8Array(size).fill(0xff), view = new DataView(bytes.buffer)
  entries.forEach((entry, index) => {
    const offset = index * 32
    bytes.fill(0, offset, offset + 32)
    view.setUint16(offset, 0x50aa, true)
    bytes[offset + 2] = entry.type
    bytes[offset + 3] = entry.subtype
    view.setUint32(offset + 4, entry.offset, true)
    view.setUint32(offset + 8, entry.size, true)
    bytes.set(new TextEncoder().encode(entry.label), offset + 12)
    view.setUint32(offset + 28, entry.flags ?? 0, true)
  })
  if (md5) {
    const offset = entries.length * 32
    bytes.set([0xeb, 0xeb], offset)
    bytes.set(createHash('md5').update(bytes.subarray(0, offset)).digest(), offset + 16)
  }
  return bytes
}

export async function installFixture() {
  const data = new Uint8Array(0x11000).fill(0xff), calls = []
  data.set([0xe9, 1, 2, 3], 0x2000)
  data.set(partitionTable(), 0x8000)
  data.set([0xe9, 6, 7, 8], 0x10000)
  const part = async (offset, size = 4) => ({ offset, bytes: size, sha256: await firmwareSha256(data.subarray(offset, offset + size)) })
  const release = { board: 'BOARD', version: 'new', image: {
    evMuxVersion: 4, chipFamily: 0x3280, chipRevision: 302, ...(await part(0x10000)),
    partitionSize: 0x10000, partitionTable: await part(0x8000, 0xc00), url: 'https://example.test/app.bin',
  }, install: { offset: 0, bytes: data.length, flashSize: 0x1000000, chipRevisionMin: 302, chipRevisionMax: 302,
    sha256: await firmwareSha256(data), bootloader: await part(0x2000), url: 'https://example.test/full.bin' } }
  const flash = new Uint8Array(0x41000).fill(0x5a)
  const loader = {
    chipFamily: 0x3280, chipRevision: 302,
    async initialize() { calls.push('initialize') },
    async getSecurityInfo() { return { flags: 0, flashCryptCnt: 0 } },
    async runStub() { calls.push('stub'); return loader },
    async flashId() { calls.push('capacity'); return 0x1840ef },
    async eraseRegion(offset, size) { calls.push(['erase', offset, size]); flash.fill(0xff, offset, offset + size) },
    async flashData(buffer, progress, offset) { calls.push(['write', offset]); flash.set(new Uint8Array(buffer), offset); progress(buffer.byteLength, buffer.byteLength) },
    async readFlash(offset, count) { calls.push(['read', offset]); return flash.slice(offset, offset + count) },
    async readRegister() { return 0 },
    async hardReset() { calls.push('reset') },
    async disconnect() { calls.push('close') },
  }
  const device = createDevice(), request = { mode: 'install', release, board: 'BOARD', data, async openLoader() { calls.push('open'); return loader } }
  return { data, calls, release, loader, device, request, flash }
}

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { FirmwareFlashSession, firmwareSha256 } from '../src/firmware_flash.js'
import { installFixture, partitionTable } from './firmware-fixtures.mjs'

const entries = () => [
  { label: 'nvs', type: 1, subtype: 2, offset: 0x9000, size: 0x6000 },
  { label: 'factory', type: 0, subtype: 0, offset: 0x10000, size: 0x10000 },
  { label: 'ffat', type: 1, subtype: 0x81, offset: 0x20000, size: 0x21000 },
]
async function replaceTable(f, raw) {
  f.data.fill(0xff, 0x8000, 0x8c00)
  f.data.set(raw, 0x8000)
  f.release.image.partitionTable.bytes = raw.length
  f.release.image.partitionTable.sha256 = await firmwareSha256(raw)
  f.release.install.sha256 = await firmwareSha256(f.data)
}
const destructive = f => f.calls.filter(call => Array.isArray(call) && ['erase', 'write'].includes(call[0]))

for (const mutation of ['no-ffat', 'legacy-vfs', 'wrong-type', 'wrong-subtype', 'encrypted', 'readonly', 'unknown-flags',
  'unaligned-offset', 'unaligned-size', 'zero-size', 'outside-flash', 'overlap', 'duplicate-label', 'wrong-app', 'reserved-range']) {
  test(`install refuses ${mutation} in a hash-approved table before opening ROM`, async () => {
    const f = await installFixture(), parts = entries(), fat = parts[2]
    if (mutation === 'no-ffat') parts.pop()
    if (mutation === 'legacy-vfs') fat.label = 'vfs'
    if (mutation === 'wrong-type') fat.type = 0x40
    if (mutation === 'wrong-subtype') fat.subtype = 0x82
    if (mutation === 'encrypted') fat.flags = 1
    if (mutation === 'readonly') fat.flags = 2
    if (mutation === 'unknown-flags') fat.flags = 4
    if (mutation === 'unaligned-offset') fat.offset++
    if (mutation === 'unaligned-size') fat.size--
    if (mutation === 'zero-size') fat.size = 0
    if (mutation === 'outside-flash') fat.size = 0x1000000
    if (mutation === 'overlap') fat.offset = 0x1f000
    if (mutation === 'duplicate-label') parts[0].label = 'ffat'
    if (mutation === 'wrong-app') parts[1].size = 0x1000
    if (mutation === 'reserved-range') parts[0].offset = 0x8000
    await replaceTable(f, partitionTable(parts))
    await assert.rejects(f.device.firmware.update(f.request), error => ['FIRMWARE_PARTITION_INVALID', 'FIRMWARE_FATFS_UNAVAILABLE'].includes(error.code))
    assert.deepEqual(f.calls, [])
  })
}

for (const mutation of ['bad-magic', 'bad-md5', 'duplicate-md5', 'entry-after-md5', 'data-after-end', 'short-table', 'missing-end', 'empty']) {
  test(`install refuses ${mutation} despite an approved outer SHA`, async () => {
    const f = await installFixture(), raw = partitionTable(entries())
    if (mutation === 'bad-magic') raw[0] ^= 1
    if (mutation === 'bad-md5') raw[112] ^= 1
    if (mutation === 'duplicate-md5') raw.copyWithin(128, 96, 128)
    if (mutation === 'entry-after-md5') raw.copyWithin(128, 0, 32)
    if (mutation === 'data-after-end') raw[200] = 0
    if (mutation === 'empty') raw.fill(0xff)
    await replaceTable(f, mutation === 'short-table' ? raw.subarray(0, 129) : mutation === 'missing-end' ? raw.subarray(0, 128) : raw)
    await assert.rejects(f.device.firmware.update(f.request), { code: 'FIRMWARE_PARTITION_INVALID' })
    assert.deepEqual(f.calls, [])
  })
}

test('historical tables without MD5 are accepted with their SHA and FF end marker', async () => {
  const f = await installFixture()
  await replaceTable(f, partitionTable(entries(), { md5: false }))
  assert.equal((await f.device.firmware.update(f.request)).verified, true)
  assert.deepEqual(destructive(f), [['erase', 0x20000, 0x21000], ['write', 0]])
})

for (const failure of ['erase', 'read', 'short-read', 'not-erased', 'missing-erase', 'missing-read', 'missing-write']) {
  test(`${failure} cannot write firmware, reset, or report complete installation`, async () => {
    const f = await installFixture(), progress = [], read = f.loader.readFlash
    if (failure === 'erase') f.loader.eraseRegion = async () => { f.calls.push(['erase', 0x20000, 0x21000]); throw new Error('erase denied') }
    if (failure === 'read') f.loader.readFlash = async () => { throw new Error('port lost') }
    if (failure === 'short-read') f.loader.readFlash = async () => new Uint8Array(1).fill(0xff)
    if (failure === 'not-erased') f.loader.readFlash = async (offset, count) => { const result = await read(offset, count); result[0] = 0; return result }
    if (failure === 'missing-erase') delete f.loader.eraseRegion
    if (failure === 'missing-read') delete f.loader.readFlash
    if (failure === 'missing-write') delete f.loader.flashData
    await assert.rejects(f.device.firmware.update({ ...f.request, onProgress: value => progress.push(value) }))
    assert.ok(!destructive(f).some(call => call[0] === 'write'))
    assert.ok(!f.calls.includes('reset'))
    assert.ok(!progress.includes(100))
    assert.equal(f.calls.at(-1), 'close')
    assert.ok(destructive(f).length <= 1, 'never retry an uncertain erase')
    if (failure.startsWith('missing-')) assert.deepEqual(destructive(f), [])
  })
}

for (const point of ['before-erase', 'during-erase', 'during-read', 'last-erase-progress', 'before-write']) {
  test(`cancellation ${point} stops installation before any firmware write`, async () => {
    const f = await installFixture(), controller = new AbortController(), reason = new Error('cancel install')
    const erase = f.loader.eraseRegion, read = f.loader.readFlash
    if (point === 'during-erase') f.loader.eraseRegion = async (...args) => { await erase(...args); controller.abort(reason) }
    if (point === 'during-read') f.loader.readFlash = async (...args) => { const result = await read(...args); controller.abort(reason); return result }
    await assert.rejects(f.device.firmware.update({ ...f.request, signal: controller.signal,
      onPhase: text => { if ((point === 'before-erase' && text.startsWith('Erasing')) || (point === 'before-write' && text.startsWith('Installing'))) controller.abort(reason) },
      onProgress: percent => { if (point === 'last-erase-progress' && percent === 20) controller.abort(reason) },
    }), error => error === reason)
    assert.ok(!destructive(f).some(call => call[0] === 'write'))
    assert.ok(!f.calls.includes('reset'))
  })
}

test('application update preserves the FAT sentinel and never reads or erases FFAT', async () => {
  const f = await installFixture(), table = f.release.image.partitionTable
  f.flash.set(f.data.subarray(table.offset, table.offset + table.bytes), table.offset)
  const before = f.flash.slice()
  await f.device.firmware.update({ ...f.request, mode: 'update', data: f.data.slice(0x10000, 0x10004) })
  assert.deepEqual(f.flash.subarray(0x20000), before.subarray(0x20000))
  assert.deepEqual(destructive(f), [['write', 0x10000]])
  assert.deepEqual(f.calls.filter(call => Array.isArray(call) && call[0] === 'read'), [['read', 0x8000], ['read', 0x10000]])
})

function installationImage(f) {
  return { ...f.release.image, ...f.release.install, application: f.release.image,
    chipRevision: undefined, chipRevisionMin: 302, chipRevisionMax: 302 }
}

test('metadata and Buffer mutations cannot change qualified erase or write ranges', async () => {
  const f = await installFixture(), original = f.data.slice(), buffer = Buffer.from(f.data)
  const image = installationImage(f), session = new FirmwareFlashSession(f.loader, image, { mode: 'install' })
  await session.connect(buffer)
  image.sha256 = '0'.repeat(64); image.flashSize = 0x4000000; image.partitionTable.offset = 0
  session.mode = 'update'
  await session.flash(buffer, { onPhase: () => buffer.fill(0) })
  assert.deepEqual(destructive(f), [['erase', 0x20000, 0x21000], ['write', 0]])
  assert.deepEqual(f.flash.slice(0, original.length), original)
})

test('a different image after connection rejects before erase even if public metadata changes', async () => {
  const f = await installFixture(), session = new FirmwareFlashSession(f.loader, installationImage(f), { mode: 'install' })
  await session.connect(f.data)
  f.data[0] ^= 1
  session.image.sha256 = await firmwareSha256(f.data)
  await assert.rejects(session.flash(f.data), /SHA-256/)
  assert.deepEqual(destructive(f), [])
})

test('secure download mode refuses installation before stub or erase', async () => {
  const f = await installFixture()
  f.loader.getSecurityInfo = async () => ({ flags: 4, flashCryptCnt: 0 })
  await assert.rejects(f.device.firmware.update(f.request), /signed flashing workflow/)
  assert.deepEqual(f.calls, ['open', 'initialize', 'close'])
})

for (const security of [{ flags: -8, flashCryptCnt: 0 }, { flags: 0, flashCryptCnt: -3 }, { flags: 0x100000000, flashCryptCnt: 0 }]) {
  test(`invalid ROM security data ${JSON.stringify(security)} rejects before erase`, async () => {
    const f = await installFixture()
    f.loader.getSecurityInfo = async () => security
    await assert.rejects(f.device.firmware.update(f.request), /security mode/)
    assert.deepEqual(f.calls, ['open', 'initialize', 'close'])
  })
}

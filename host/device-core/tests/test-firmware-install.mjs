// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { installFixture as fixture } from './firmware-fixtures.mjs'

test('blank device installs through ROM without an EV-MUX transport or existing partition table', async () => {
  const f = await fixture(), progress = []
  const result = await f.device.firmware.update({ ...f.request, onProgress: value => progress.push(value) })
  assert.equal(result.verified, true); assert.equal(result.reconnected, false)
  assert.deepEqual(f.flash.slice(0, f.data.length), f.data)
  assert.ok(f.flash.subarray(0x20000).every(value => value === 0xff))
  assert.deepEqual(f.calls, ['open', 'initialize', 'stub', 'capacity', ['erase', 0x20000, 0x21000], ['read', 0x20000], ['read', 0x30000], ['read', 0x40000], ['write', 0], ['read', 0], ['read', 65536], 'reset', 'close'])
  assert.equal(progress.at(-1), 100)
})

test('default application update refuses blank layout and never falls back to installing', async () => {
  const f = await fixture()
  await assert.rejects(f.device.firmware.update({ ...f.request, mode: undefined, data: f.data.slice(65536, 65540) }), /Partition layout/)
  assert.ok(!f.calls.some(call => Array.isArray(call) && call[0] === 'write'))
  assert.ok(!f.calls.includes('capacity'))
})

test('complete image is downloaded from install URL with the requested cancellation signal', async () => {
  const f = await fixture(), controller = new AbortController()
  await f.device.firmware.update({ ...f.request, data: undefined, signal: controller.signal,
    async fetch(url, options) { assert.equal(url, f.release.install.url); assert.equal(options.signal, controller.signal); return { ok: true, arrayBuffer: async () => f.data.buffer } },
  })
  assert.deepEqual(f.flash.slice(0, f.data.length), f.data)
})

test('install selection skips app-only releases and rejects explicit unqualified versions', async () => {
  const f = await fixture(), appOnly = { ...f.release, version: 'app-only', install: undefined }
  const request = { mode: 'install', identity: { board: 'BOARD', firmwareVersion: 'unknown' }, catalog: { schemaVersion: 1, releases: [appOnly, f.release] } }
  const result = await f.device.firmware.check(request)
  assert.equal(result.release.version, 'new'); assert.equal(result.releases.length, 2)
  await assert.rejects(f.device.firmware.check({ ...request, version: 'app-only' }), { code: 'FIRMWARE_INSTALL_UNAVAILABLE' })
  const updates = await f.device.firmware.check({ ...request, mode: 'update' })
  assert.equal(updates.release.version, 'app-only')
  assert.deepEqual(f.calls, [])
})

for (const change of ['hash', 'bootloader', 'table', 'application', 'overlap', 'bounds', 'revision-range', 'missing-metadata', 'mode', 'board']) {
  test(`invalid ${change} rejects installation before opening or releasing a port`, async () => {
    const f = await fixture()
    if (change === 'hash') f.data[1] = 0
    if (change === 'bootloader') f.release.install.bootloader.sha256 = '0'.repeat(64)
    if (change === 'table') f.release.image.partitionTable.sha256 = '0'.repeat(64)
    if (change === 'application') f.release.image.sha256 = '0'.repeat(64)
    if (change === 'overlap') f.release.install.bootloader.bytes = 0x8000
    if (change === 'bounds') f.release.install.offset = 0x1000
    if (change === 'revision-range') f.release.install.chipRevisionMin = 301
    if (change === 'missing-metadata') delete f.release.install
    if (change === 'mode') f.request.mode = 'auto'
    if (change === 'board') f.request.board = 'WRONG'
    await assert.rejects(f.device.firmware.update(f.request))
    assert.deepEqual(f.calls, [])
  })
}

for (const change of ['wrong-chip', 'wrong-revision', 'secure-boot', 'encryption', 'small-flash', 'unknown-flash', 'missing-capacity']) {
  test(`physical ${change} refuses installation before any write`, async () => {
    const f = await fixture()
    if (change === 'wrong-chip') f.loader.chipFamily = 0x3253
    if (change === 'wrong-revision') f.loader.chipRevision = 301
    if (change === 'secure-boot') f.loader.getSecurityInfo = async () => ({ flags: 1, flashCryptCnt: 0 })
    if (change === 'encryption') f.loader.getSecurityInfo = async () => ({ flags: 0, flashCryptCnt: 1 })
    if (change === 'small-flash') f.loader.flashId = async () => 0x1740ef
    if (change === 'unknown-flash') f.loader.flashId = async () => 0xffffff
    if (change === 'missing-capacity') delete f.loader.flashId
    await assert.rejects(f.device.firmware.update(f.request))
    assert.ok(!f.calls.some(call => Array.isArray(call) && ['write', 'erase'].includes(call[0])))
    assert.equal(f.calls.at(-1), 'close')
  })
}

test('installation with damaged readback does not reset or report success', async () => {
  const f = await fixture(), read = f.loader.readFlash
  f.loader.readFlash = async (offset, count) => offset >= 0x20000 ? read(offset, count) : new Uint8Array(count)
  await assert.rejects(f.device.firmware.update(f.request), /verification failed/)
  assert.ok(!f.calls.includes('reset')); assert.equal(f.calls.at(-1), 'close')
})

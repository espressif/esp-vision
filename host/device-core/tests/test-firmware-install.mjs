// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createDevice } from '../src/index.js'
import { firmwareSha256 } from '../src/firmware_flash.js'
import { parseCli, runCli } from '../src/cli.js'

async function fixture() {
  const data = new Uint8Array(0x11000).fill(0xff), calls = []
  data.set([0xe9, 1, 2, 3], 0x2000)
  data.set([0xaa, 0x50, 4, 5], 0x8000)
  data.set([0xe9, 6, 7, 8], 0x10000)
  const part = async offset => ({ offset, bytes: 4, sha256: await firmwareSha256(data.subarray(offset, offset + 4)) })
  const release = { board: 'BOARD', version: 'new', image: {
    evMuxVersion: 4, chipFamily: 0x3280, chipRevision: 302, ...(await part(0x10000)),
    partitionSize: 4096, partitionTable: await part(0x8000), url: 'https://example.test/app.bin',
  }, install: { offset: 0, bytes: data.length, flashSize: 0x1000000, chipRevisionMin: 302, chipRevisionMax: 302,
    sha256: await firmwareSha256(data), bootloader: await part(0x2000), url: 'https://example.test/full.bin' } }
  const flash = new Uint8Array(data.length).fill(0xff)
  const loader = {
    chipFamily: 0x3280, chipRevision: 302,
    async initialize() { calls.push('initialize') },
    async getSecurityInfo() { return { flags: 0, flashCryptCnt: 0 } },
    async runStub() { calls.push('stub'); return loader },
    async flashId() { calls.push('capacity'); return 0x1840ef },
    async flashData(buffer, progress, offset) { calls.push(['write', offset]); flash.set(new Uint8Array(buffer), offset); progress(buffer.byteLength, buffer.byteLength) },
    async readFlash(offset, count) { calls.push(['read', offset]); return flash.slice(offset, offset + count) },
    async readRegister() { return 0 },
    async hardReset() { calls.push('reset') },
    async disconnect() { calls.push('close') },
  }
  const device = createDevice(), request = { mode: 'install', release, board: 'BOARD', data, async openLoader() { calls.push('open'); return loader } }
  return { data, calls, release, loader, device, request, flash }
}

test('blank device installs through ROM without an EV-MUX transport or existing partition table', async () => {
  const f = await fixture(), progress = []
  const result = await f.device.firmware.update({ ...f.request, onProgress: value => progress.push(value) })
  assert.equal(result.verified, true); assert.equal(result.reconnected, false)
  assert.deepEqual(f.flash, f.data)
  assert.deepEqual(f.calls, ['open', 'initialize', 'stub', 'capacity', ['write', 0], ['read', 0], ['read', 65536], 'reset', 'close'])
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
  assert.deepEqual(f.flash, f.data)
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
    assert.ok(!f.calls.some(call => Array.isArray(call) && call[0] === 'write'))
    assert.equal(f.calls.at(-1), 'close')
  })
}

test('installation with damaged readback does not reset or report success', async () => {
  const f = await fixture(); f.loader.readFlash = async (_offset, count) => new Uint8Array(count)
  await assert.rejects(f.device.firmware.update(f.request), /verification failed/)
  assert.ok(!f.calls.includes('reset')); assert.equal(f.calls.at(-1), 'close')
})

test('CLI install uses ROM directly and passes mode through both check and update', async () => {
  const f = await fixture(), checked = []
  const options = parseCli(['firmware', 'update', '--mode', 'install', '--board', 'BOARD', '--port', '/dev/ttyACM0', '--version', 'new'])
  const result = await runCli(options, {
    transportFor: () => { throw new Error('No application transport should be acquired') },
    async readFile() { throw new Error('No user catalog required') },
    createDevice: () => ({ firmware: {
      async check(request) { checked.push(request.mode); return { release: f.release } },
      async update(request) { assert.equal(request.mode, 'install'); return { verified: true } },
    }, async disconnect() {} }),
  })
  assert.equal(result.verified, true); assert.deepEqual(checked, ['install'])
  assert.throws(() => parseCli(['firmware', 'update', '--mode', 'install', '--port', 'COM3', '--version', 'new']), /explicit --board/)
  assert.throws(() => parseCli(['firmware', 'update', '--mode', 'install', '--tcp', 'host', '--board', 'BOARD', '--version', 'new']), /local --port/)
})

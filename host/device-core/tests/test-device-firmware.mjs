/* SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createDevice } from '../src/index.js'
import { firmwareSha256 } from '../src/firmware_flash.js'

async function fixture() {
  const data = new Uint8Array([0xe9, 1, 2, 3]), table = new Uint8Array([4, 5]), calls = []
  const release = { board: 'BOARD', version: '2026.10.15', image: { evMuxVersion: 4, chipFamily: 0x3280, chipRevision: 302, offset: 65536, bytes: 4, partitionSize: 4096, sha256: await firmwareSha256(data), partitionTable: { offset: 32768, bytes: 2, sha256: await firmwareSha256(table) }, url: 'https://example.test/app.bin' } }
  const stub = { async readFlash(offset) { return offset === 32768 ? table : data }, async readRegister() { return 0 }, async flashData() { calls.push('write') } }
  const loader = { chipFamily: 0x3280, chipRevision: 302, async initialize() { calls.push('initialize') }, async getSecurityInfo() { return { flags: 0, flashCryptCnt: 0 } }, async runStub() { return stub }, async hardReset() { calls.push('reset') }, async disconnect() { calls.push('close') } }
  const device = createDevice(), request = { release, board: 'BOARD', data, async openLoader() { calls.push('open'); return loader } }
  return { device, release, request, data, calls, loader, stub }
}
test('firmware check works offline and preserves catalog order for calendar/git versions', async () => {
  const f = await fixture(), identity = { board: 'BOARD', firmwareVersion: '2026.08.27-18-gabc' }
  const catalog = { schemaVersion: 1, releases: [f.release, { ...f.release, version: '0.9' }] }
  const result = await f.device.firmware.check({ catalog, identity })
  assert.equal(result.available, true); assert.equal(result.release.version, '2026.10.15')
  result.release.image.sha256 = 'x'; assert.notEqual(catalog.releases[0].image.sha256, 'x')
  assert.deepEqual(f.calls, [])
})
test('firmware check distinguishes current firmware and unavailable board', async () => {
  const f = await fixture(), catalog = { schemaVersion: 1, releases: [f.release] }
  const current = await f.device.firmware.check({ catalog, identity: { board: 'BOARD', firmwareVersion: f.release.version } })
  assert.equal(current.available, false); assert.ok(current.release)
  const missing = await f.device.firmware.check({ catalog, identity: { board: 'OTHER', firmwareVersion: 'v1' } })
  assert.equal(missing.available, false); assert.equal(missing.release, null)
})
test('firmware catalog download propagates cancellation and HTTP errors without connecting', async () => {
  const f = await fixture(), controller = new AbortController()
  const request = { catalog: 'https://example.test/releases.json', identity: { board: 'BOARD', firmwareVersion: 'v1' }, signal: controller.signal,
    async fetch(url, options) { assert.equal(options.signal, controller.signal); assert.equal(options.cache, 'no-store'); return { ok: false, status: 503 } } }
  await assert.rejects(f.device.firmware.check(request), { code: 'FIRMWARE_DOWNLOAD_FAILED' })
  controller.abort(); await assert.rejects(f.device.firmware.check(request), { name: 'AbortError' })
  assert.deepEqual(f.calls, [])
})
test('invalid catalog, wrong protocol and unknown arguments are rejected', async () => {
  const f = await fixture(), identity = { board: 'BOARD', firmwareVersion: 'v1' }
  for (const catalog of [{}, { schemaVersion: 1, releases: [{ ...f.release, image: { ...f.release.image, evMuxVersion: 3 } }] }]) await assert.rejects(f.device.firmware.check({ catalog, identity }), TypeError)
  await assert.rejects(f.device.firmware.check({ catalog: 'http://example.test/releases.json', identity }), /HTTPS/)
  await assert.rejects(f.device.firmware.check({ identity, typo: true }), /Unknown/)
})
test('firmware update verifies image and physical board before opening ROM', async () => {
  const f = await fixture()
  await assert.rejects(f.device.firmware.update({ ...f.request, board: 'OTHER' }), /board/)
  await assert.rejects(f.device.firmware.update({ ...f.request, data: new Uint8Array(4) }), /SHA-256/)
  assert.deepEqual(f.calls, [])
})
test('firmware update uses application-only verified ROM flow and returns explicit reconnect state', async () => {
  const f = await fixture(), result = await f.device.firmware.update(f.request)
  assert.deepEqual(result, { verified: true, needsReset: false, board: 'BOARD', version: f.release.version, reconnected: false })
  assert.deepEqual(f.calls, ['open', 'initialize', 'write', 'reset', 'close'])
})
test('firmware update downloads raw application image through injected fetch', async () => {
  const f = await fixture()
  await f.device.firmware.update({ ...f.request, data: undefined, async fetch(url) { assert.equal(url, f.release.image.url); return { ok: true, async arrayBuffer() { return f.data.buffer } } } })
  assert.ok(f.calls.includes('write'))
})
test('cancel after loader acquisition closes port without writing', async () => {
  const f = await fixture(), controller = new AbortController()
  await assert.rejects(f.device.firmware.update({ ...f.request, signal: controller.signal, async openLoader() { controller.abort(); return f.loader } }), { name: 'AbortError' })
  assert.deepEqual(f.calls, ['close'])
})
test('active update excludes connect and concurrent update; failures release the reservation', async () => {
  const f = await fixture(); let unblock
  const waiting = new Promise(resolve => { unblock = resolve })
  const active = f.device.firmware.update({ ...f.request, async openLoader() { await waiting; throw new Error('port unavailable') } })
  await assert.rejects(f.device.connect(), { code: 'DEVICE_BUSY' })
  await assert.rejects(f.device.firmware.update(f.request), { code: 'DEVICE_BUSY' })
  unblock(); await assert.rejects(active, /port unavailable/)
  assert.equal((await f.device.firmware.update(f.request)).verified, true)
})
test('failed readback never reports verified or resets the device', async () => {
  const f = await fixture(); f.stub.readFlash = async offset => offset === 32768 ? new Uint8Array([4, 5]) : new Uint8Array(4)
  await assert.rejects(f.device.firmware.update(f.request), /verification failed/)
  assert.ok(!f.calls.includes('reset')); assert.equal(f.calls.at(-1), 'close')
})
test('reset failure returns verified with explicit manual reset requirement', async () => {
  const f = await fixture(); f.loader.hardReset = async () => { throw new Error('reset unavailable') }
  assert.equal((await f.device.firmware.update(f.request)).needsReset, true)
})

test('firmware check lists all board versions and selects an explicit older version', async () => {
  const f = await fixture(), older = { ...f.release, version: '2026.08.27' }
  const request = { catalog: { schemaVersion: 1, releases: [f.release, { ...f.release, board: 'OTHER' }, older] }, identity: { board: 'BOARD', firmwareVersion: f.release.version } }
  const history = await f.device.firmware.check(request)
  assert.deepEqual(history.releases.map(item => item.version), [f.release.version, older.version])
  assert.equal(history.available, false)
  const selected = await f.device.firmware.check({ ...request, version: older.version })
  assert.equal(selected.available, true); assert.equal(selected.release.version, older.version)
  assert.equal((await f.device.firmware.update({ ...f.request, release: selected.release })).version, older.version)
})
test('an unavailable or ambiguous requested version never falls back to the latest build', async () => {
  const f = await fixture(), request = { catalog: { schemaVersion: 1, releases: [f.release] }, identity: { board: 'BOARD', firmwareVersion: 'old' } }
  await assert.rejects(f.device.firmware.check({ ...request, version: 'missing' }), { code: 'FIRMWARE_VERSION_UNAVAILABLE' })
  for (const version of ['', 4, null]) await assert.rejects(f.device.firmware.check({ ...request, version }), /version must/)
  request.catalog.releases.push({ ...f.release })
  await assert.rejects(f.device.firmware.check(request), /duplicate versions/)
  assert.deepEqual(f.calls, [])
})

test('default update check reads official dl config and lists legacy releases without enabling unqualified writes', async () => {
  const f = await fixture(), urls = []
  const result = await f.device.firmware.check({ identity: { board: 'BOARD', firmwareVersion: 'old' }, async fetch(url) {
    urls.push(url)
    if (url.endsWith('config.toml')) return { ok: true, text: async () => 'firmware_images_url = "https://dl.espressif.com/AE/esp-vision/bin/"\n[esp-vision.image]\nBOARD = "latest.bin"\n[website]\nrelease = "new"\nreleases = ["new", "old"]\nbinaryNameTemplate = "esp-vision-{board}-{tag}.bin"\n' }
    if (url.endsWith('/new.json')) return { ok: true, json: async () => ({ ...f.release, version: 'new', image: { ...f.release.image, url: 'https://dl.espressif.com/AE/esp-vision/bin/new-app.bin' } }) }
    return { ok: false, status: 404 }
  } })
  assert.equal(result.available, true); assert.equal(result.release.version, 'new')
  assert.equal(result.releases.length, 2); assert.equal(result.releases[1].compatible, false)
  assert.equal(urls[0], 'https://dl.espressif.com/AE/esp-vision/config.toml')
})
test('unqualified historical version remains visible but is never selected for flashing', async () => {
  const f = await fixture(), request = { identity: { board: 'BOARD', firmwareVersion: 'x' }, catalog: { schemaVersion: 1, releases: [{ board: 'BOARD', version: 'old', compatible: false, reason: 'UPDATE_METADATA_UNAVAILABLE' }] } }
  const result = await f.device.firmware.check(request)
  assert.equal(result.available, false); assert.equal(result.release, null); assert.equal(result.releases.length, 1)
  await assert.rejects(f.device.firmware.check({ ...request, version: 'old' }), { code: 'FIRMWARE_INCOMPATIBLE' })
  await assert.rejects(f.device.firmware.update({ ...f.request, release: result.releases[0] }), /metadata/)
  assert.deepEqual(f.calls, [])
})
test('approved silicon revision interval is checked before any write', async () => {
  const f = await fixture(); delete f.release.image.chipRevision
  f.release.image.chipRevisionMin = 301; f.release.image.chipRevisionMax = 399
  assert.equal((await f.device.firmware.update(f.request)).verified, true)
  const bad = await fixture(); delete bad.release.image.chipRevision
  bad.release.image.chipRevisionMin = 303; bad.release.image.chipRevisionMax = 399
  await assert.rejects(bad.device.firmware.update(bad.request), /revision/)
  assert.ok(!bad.calls.includes('write'))
})

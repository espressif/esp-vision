// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { createDevice } from '../src/index.js'
import { firmwareSha256 } from '../src/firmware.js'

async function fixture() {
  const full = new Uint8Array(0x11000).fill(0xff), appOffset = 0x10000, appLength = 4, calls = []
  full.set([0xe9, 1, 2, 3], 0x2000); full.set([0xaa, 0x50, 4, 5], 0x8000); full.set([0xe9, 6, 7, 8], appOffset)
  const segment = async offset => ({ offset, bytes: 4, sha256: await firmwareSha256(full.subarray(offset, offset + 4)) })
  const source = { url: 'https://example.test/full.bin', bytes: full.length, sha256: await firmwareSha256(full), offset: appOffset }
  const release = { board: 'BOARD', version: 'new', image: {
    evMuxVersion: 4, chipFamily: 0x3280, chipRevision: 302, ...(await segment(appOffset)), partitionSize: 4096,
    partitionTable: await segment(0x8000), url: source.url, source,
  }, install: { url: source.url, offset: 0, bytes: full.length, sha256: source.sha256, flashSize: 0x1000000,
    chipRevisionMin: 302, chipRevisionMax: 302, bootloader: await segment(0x2000) } }
  const flash = new Uint8Array(full.length).fill(0x5a)
  flash.set(full.subarray(0x8000, 0x8004), 0x8000)
  const before = flash.slice()
  const loader = {
    chipFamily: 0x3280, chipRevision: 302,
    async initialize() { calls.push('initialize') },
    async getSecurityInfo() { return { flags: 0, flashCryptCnt: 0 } },
    async runStub() { calls.push('stub'); return loader },
    async flashId() { return 0x1840ef },
    async flashData(buffer, progress, offset) { calls.push(['write', offset, buffer.byteLength]); flash.set(new Uint8Array(buffer), offset); progress(buffer.byteLength, buffer.byteLength) },
    async readFlash(offset, bytes) { return flash.slice(offset, offset + bytes) },
    async readRegister() { return 0 },
    async hardReset() { calls.push('reset') },
    async disconnect() { calls.push('close') },
  }
  const request = { release, board: 'BOARD', openLoader: async () => { calls.push('open'); return loader }, fetch: async (url, options) => {
    calls.push(['download', url]); assert.equal(options.redirect, 'error'); assert.equal(options.headers, undefined)
    return new Response(full)
  } }
  return { device: createDevice(), release, request, full, flash, before, loader, calls, appOffset, appLength }
}
test('single BIN update verifies the container then writes only the application, preserving all other bytes', async () => {
  const f = await fixture()
  const result = await f.device.firmware.update(f.request)
  assert.equal(result.verified, true)
  assert.deepEqual(f.calls.filter(call => Array.isArray(call)), [['download', f.release.image.source.url], ['write', f.appOffset, f.appLength]])
  assert.deepEqual(f.flash.slice(0, f.appOffset), f.before.slice(0, f.appOffset))
  assert.deepEqual(f.flash.slice(f.appOffset + f.appLength), f.before.slice(f.appOffset + f.appLength))
  assert.deepEqual(f.flash.slice(f.appOffset, f.appOffset + f.appLength), f.full.slice(f.appOffset, f.appOffset + f.appLength))
})
test('provided data means the complete BIN when image.source exists; application bytes alone are rejected', async () => {
  const f = await fixture()
  await assert.rejects(f.device.firmware.update({ ...f.request, data: f.full.slice(f.appOffset, f.appOffset + f.appLength) }), /source size/)
  assert.deepEqual(f.calls, [])
  assert.equal((await f.device.firmware.update({ ...f.request, data: f.full })).verified, true)
  assert.ok(!f.calls.some(call => Array.isArray(call) && call[0] === 'download'))
})
test('old application-only descriptors still download and verify a raw application BIN', async () => {
  const f = await fixture(); delete f.release.image.source; f.release.image.url = 'https://example.test/app.bin'
  let downloads = 0
  const result = await f.device.firmware.update({ ...f.request, fetch: async url => {
    downloads++; assert.equal(url, f.release.image.url); return new Response(f.full.slice(f.appOffset, f.appOffset + f.appLength))
  } })
  assert.equal(result.verified, true); assert.equal(downloads, 1)
})
for (const mutation of ['outside-app-corruption', 'source-truncated', 'source-long', 'app-hash', 'source-offset', 'source-bounds', 'source-size-limit', 'source-install-hash', 'source-install-url', 'image-source-url', 'source-http', 'bootloader-descriptor', 'table-descriptor']) {
  test(`invalid source ${mutation} rejects before any ROM acquisition`, async () => {
    const f = await fixture(), source = f.release.image.source
    let bytes = f.full
    if (mutation === 'outside-app-corruption') bytes[1] ^= 1
    if (mutation === 'source-truncated') bytes = bytes.slice(0, -1)
    if (mutation === 'source-long') bytes = new Uint8Array(bytes.length + 1)
    if (mutation === 'app-hash') f.release.image.sha256 = '0'.repeat(64)
    if (mutation === 'source-offset') source.offset++
    if (mutation === 'source-bounds') source.bytes = 1
    if (mutation === 'source-size-limit') source.bytes = 257 * 1024 * 1024
    if (mutation === 'source-install-hash') source.sha256 = '0'.repeat(64)
    if (mutation === 'source-install-url') f.release.install.url = 'https://example.test/other.bin'
    if (mutation === 'image-source-url') f.release.image.url = 'https://example.test/app.bin'
    if (mutation === 'bootloader-descriptor') f.release.install.bootloader.sha256 = '0'.repeat(64)
    if (mutation === 'table-descriptor') f.release.image.partitionTable.sha256 = '0'.repeat(64)
    if (mutation === 'source-http') source.url = f.release.image.url = f.release.install.url = 'http://example.test/full.bin'
    await assert.rejects(f.device.firmware.update({ ...f.request, data: bytes }))
    assert.deepEqual(f.calls, [])
  })
}
test('downloaded full BIN enforces approved length during streaming before opening ROM', async () => {
  const f = await fixture()
  await assert.rejects(f.device.firmware.update({ ...f.request, fetch: async () => new Response(new Uint8Array(f.full.length + 1)) }), /byte limit/)
  assert.deepEqual(f.calls, [])
})
test('single BIN explicit install writes the complete container without slicing it', async () => {
  const f = await fixture()
  assert.equal((await f.device.firmware.update({ ...f.request, data: f.full, mode: 'install' })).verified, true)
  assert.deepEqual(f.flash, f.full)
  assert.deepEqual(f.calls.find(call => Array.isArray(call) && call[0] === 'write'), ['write', 0, f.full.length])
})
test('single BIN application update still refuses changed partition layouts with no install fallback', async () => {
  const f = await fixture(); f.flash[0x8000] ^= 1
  await assert.rejects(f.device.firmware.update({ ...f.request, data: f.full }), /Partition layout/)
  assert.ok(!f.calls.some(call => Array.isArray(call) && call[0] === 'write'))
  assert.equal(f.calls.at(-1), 'close')
})
test('source extraction retains complete application readback verification and does not reset on corruption', async () => {
  const f = await fixture(), read = f.loader.readFlash
  f.loader.readFlash = async (offset, count) => offset === 0x8000 ? read(offset, count) : new Uint8Array(count)
  await assert.rejects(f.device.firmware.update({ ...f.request, data: f.full }), /verification failed/)
  assert.ok(!f.calls.includes('reset')); assert.equal(f.calls.at(-1), 'close')
})

test('single BIN update and explicit installation reuse hash-pinned browser-cache responses', async () => {
  for (const mode of ['update', 'install']) {
    const f = await fixture(), caches = []
    const result = await f.device.firmware.update({ ...f.request, mode, fetch: async (_url, options) => {
      caches.push(options.cache)
      return new Response(f.full)
    } })
    assert.equal(result.verified, true)
    assert.deepEqual(caches, ['default'])
  }
})
test('single BIN cached CORS failure retries once and retains application-only verification', async () => {
  const f = await fixture(), caches = []
  const result = await f.device.firmware.update({ ...f.request, fetch: async (_url, options) => {
    caches.push(options.cache)
    if (caches.length === 1) throw new TypeError('Cached response lacks CORS permission')
    return new Response(f.full)
  } })
  assert.equal(result.verified, true)
  assert.deepEqual(caches, ['default', 'no-store'])
  assert.deepEqual(f.calls.find(call => Array.isArray(call) && call[0] === 'write'), ['write', f.appOffset, f.appLength])
})
test('single BIN hash mismatch never retries cache or opens ROM; legacy app BIN stays no-store', async () => {
  const f = await fixture(), caches = []
  f.full[0] ^= 1
  await assert.rejects(f.device.firmware.update({ ...f.request, fetch: async (_url, options) => {
    caches.push(options.cache); return new Response(f.full)
  } }), /SHA-256/)
  assert.deepEqual(caches, ['default']); assert.deepEqual(f.calls, [])
  delete f.release.image.source
  caches.length = 0
  await assert.rejects(f.device.firmware.update({ ...f.request, fetch: async (_url, options) => {
    caches.push(options.cache); throw new TypeError('Network failure')
  } }), /Network failure/)
  assert.deepEqual(caches, ['no-store']); assert.deepEqual(f.calls, [])
})

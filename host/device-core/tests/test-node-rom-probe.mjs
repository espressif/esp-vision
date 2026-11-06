// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { probeNodeRom } from '../src/node_rom_probe.js'

const mac = '30eda0eda1b0'
const rom = { path: '/dev/rom', vendorId: 0x303a, productId: 32, serialNumber: mac, location: '1-2:1.0' }
const runtime = { ...rom, path: '/dev/runtime', productId: 0x4023 }
function fixture(options = {}) {
  const calls = []
  let clock = 0, reset = false, register = 0x1234567c, closed = 0
  const controller = new AbortController()
  const loader = {
    chipMac: options.mac ?? mac, chipFamily: 0x3231,
    async initialize() { calls.push('initialize'); await preparation(); if (options.initializeError) throw options.initializeError },
    async getSecurityInfo() { calls.push('security'); return options.security ?? { flags: 0, flashCryptCnt: 0 } },
    async readRegister(address) { calls.push(['read', address]); return register },
    async writeRegister(address, value, mask) { calls.push(['write', address, value, mask]); if (!options.badLatch) register = (register & ~mask) >>> 0 },
    async hardReset() { calls.push('reset'); reset = true; if (options.cancel) controller.abort(new Error('cancelled')); if (options.resetError) throw options.resetError },
    async disconnect() { closed++; calls.push('close') },
  }
  let preparation
  const dependencies = {
    now: () => clock,
    async sleep(ms) { clock += ms },
    async listSerial() {
      if (!reset) return options.changedBeforeOpen ? [runtime] : [rom]
      if (options.enumerationDelay) clock += options.enumerationDelay
      return options.after ?? [runtime]
    },
    openNodeRom(request, dependencies) { calls.push('open'); preparation = dependencies.preparePort; assert.equal(request.port, rom.path); return loader },
  }
  return { calls, loader, controller, closed: () => closed,
    run: (selected = rom) => probeNodeRom(selected, { python: 'python', timeout: 200, signal: controller.signal }, dependencies) }
}

test('one normal SPI reset follows MAC, security and masked latch checks without a stub or flash access', async () => {
  const f = fixture(), result = await f.run()
  assert.equal(result.port.path, runtime.path)
  assert.equal(result.probe.outcome, 'runtime')
  assert.deepEqual(f.calls, ['open', 'initialize', 'security', ['read', 0x20700008], ['write', 0x20700008, 0, 4], ['read', 0x20700008], 'reset', 'close'])
  assert.equal(f.closed(), 1)
})

for (const [name, options, code] of [
  ['wrong physical MAC', { mac: '30eda0eda1b1' }, 'ROM_IDENTITY_CHANGED'],
  ['secure boot', { security: { flags: 1, flashCryptCnt: 0 } }, 'ROM_SECURITY_UNSUPPORTED'],
  ['encryption', { security: { flags: 0, flashCryptCnt: 1 } }, 'ROM_SECURITY_UNSUPPORTED'],
  ['bad latch', { badLatch: true }, undefined],
]) {
  test(`${name} cannot authorize a reset`, async () => {
    const f = fixture(options)
    await assert.rejects(f.run(), error => !code || error.code === code)
    assert.ok(!f.calls.includes('reset'))
    assert.equal(f.closed(), 1)
  })
}

test('a ROM snapshot that already became the matching runtime needs no additional reset', async () => {
  const f = fixture({ changedBeforeOpen: true })
  const result = await f.run()
  assert.equal(result.probe.attempted, false)
  assert.equal(result.probe.outcome, 'runtime')
  assert.equal(result.port.path, runtime.path)
  assert.ok(!f.calls.includes('reset'))
})

test('missing MAC/location rejects before opening a port', async () => {
  for (const extra of [{ serialNumber: null }, { location: null }]) {
    const f = fixture()
    await assert.rejects(f.run({ ...rom, ...extra }), { code: 'ROM_PORT_IDENTITY_REQUIRED' })
    assert.deepEqual(f.calls, [])
  }
})

test('no runtime or a return to ROM remains unknown firmware presence without reset replay', async () => {
  for (const after of [[], [rom]]) {
    const f = fixture({ after }), result = await f.run()
    assert.equal(result.probe.outcome, after.length ? 'rom' : 'no-runtime')
    assert.equal(f.calls.filter(call => call === 'reset').length, 1)
    assert.equal(f.closed(), 1)
  }
})

test('another device or physical location is never accepted as the reset device', async () => {
  for (const extra of [{ serialNumber: '30eda0eda1b1' }, { location: '1-3' }]) {
    const f = fixture({ after: [{ ...runtime, ...extra }] })
    assert.equal((await f.run()).probe.outcome, 'no-runtime')
  }
})

test('a lost reset response is retained while the matching runtime may still be observed', async () => {
  const f = fixture({ resetError: Object.assign(new Error('USB detached'), { code: 'ROM_CLOSED' }) })
  const result = await f.run()
  assert.equal(result.probe.outcome, 'runtime')
  assert.equal(result.probe.resetError.code, 'ROM_CLOSED')
  assert.equal(f.calls.filter(call => call === 'reset').length, 1)
})

test('a late enumeration cannot accept a runtime after the deadline', async () => {
  const f = fixture({ enumerationDelay: 201 })
  assert.equal((await f.run()).probe.outcome, 'no-runtime')
})

test('cancellation cleans up without another normal reset or download request', async () => {
  const before = fixture(); before.controller.abort(new Error('cancelled'))
  await assert.rejects(before.run(), /cancelled/)
  assert.deepEqual(before.calls, [])
  const during = fixture({ cancel: true })
  await assert.rejects(during.run(), /cancelled/)
  assert.equal(during.calls.filter(call => call === 'reset').length, 1)
  assert.equal(during.closed(), 1)
})

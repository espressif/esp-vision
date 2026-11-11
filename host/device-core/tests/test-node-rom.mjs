// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { openNodeRom } from '../src/node_rom.js'
function fixture({ error, chipMac = '001122334455', selected = { port: 'COM9', expectedMac: '001122334455' }, descriptors = [] } = {}) {
  const events = [], stub = { IS_STUB: true }
  const raw = {
    chipFamily: 0x3280, chipRevision: 302, chipMac,
    async initialize() { events.push('initialize'); if (error) throw error },
    async getSecurityInfo() { events.push('security'); return { flags: 0, flashCryptCnt: 0 } },
    async runStub() { events.push('stub'); return stub },
    async readRegister(address) { events.push(['read', address]); return 4 },
    async writeRegister(...args) { events.push(['write', ...args]) },
    async hardReset() { events.push('reset') },
    async disconnect() { events.push('disconnect') },
  }
  const dependencies = {
    async preparePort() { events.push('prepare'); return selected }, listSerial: async () => descriptors,
    createPort: info => ({ getInfo: () => ({ usbVendorId: info.vendorId, usbProductId: info.productId }), async close() { events.push('close-port') } }),
    createLoader: (_port, _logger, options) => { events.push(['loader', options]); return raw },
  }
  return { events, raw, stub, dependencies, loader: openNodeRom({ port: 'COM9' }, dependencies) }
}
test('passive Node adapter delegates ROM commands to shared JS loader', async () => {
  const f = fixture(); assert.deepEqual(f.events, [])
  await assert.rejects(f.loader.getSecurityInfo(), { code: 'ROM_NOT_INITIALIZED' })
  await f.loader.initialize(); assert.equal(f.loader.chipMac, '001122334455')
  assert.equal(f.events.find(Array.isArray)[1].connectMode, 'no_reset')
  assert.deepEqual(await f.loader.getSecurityInfo(), { flags: 0, flashCryptCnt: 0 })
  assert.equal(await f.loader.runStub(), f.loader); assert.equal(f.loader.IS_STUB, true)
  await f.loader.hardReset(); await f.loader.disconnect(); await f.loader.disconnect()
  assert.equal(f.events.filter(value => value === 'disconnect').length, 1)
  await assert.rejects(f.loader.initialize(), { code: 'ROM_CLOSED' })
})
test('normal boot can use ROM registers without starting a stub', async () => {
  const f = fixture(); await f.loader.initialize()
  assert.equal(await f.loader.readRegister(16), 4)
  await f.loader.writeRegister(16, 0, 4); await f.loader.hardReset(); await f.loader.disconnect()
  assert.equal(f.events.includes('stub'), false)
  assert.deepEqual(f.events.filter(Array.isArray).slice(-2), [['read', 16], ['write', 16, 0, 4]])
})
test('Node retains large reads through bounded shared-engine commands and reports complete progress', async () => {
  const f = fixture(), reads = [], progress = []
  f.stub.readFlash = async (address, size) => { reads.push([address, size]); return new Uint8Array(size).fill(address === 4096 ? 1 : 2) }
  await f.loader.initialize(); await f.loader.runStub()
  const bytes = await f.loader.readFlash(4096, 65540, (_chunk, done, total) => progress.push([done, total]), { blockSize: 4096, maxInFlight: 4 })
  assert.deepEqual(reads, [[4096, 65536], [69632, 4]])
  assert.deepEqual(progress, [[65536, 65540], [65540, 65540]])
  assert.equal(bytes[65535], 1); assert.equal(bytes[65536], 2)
  await assert.rejects(f.loader.readFlash(0, 65540, undefined, { chunkSize: 4096 }), /chunk size/)
  await f.loader.disconnect()
})
test('identity mismatch fails before security, stub or writes', async () => {
  const f = fixture({ chipMac: 'ffffffffffff' })
  await assert.rejects(f.loader.initialize(), { code: 'ROM_IDENTITY_CHANGED' })
  assert.equal(f.events.includes('stub'), false); assert.equal(f.events.includes('security'), false)
  assert.ok(f.events.includes('close-port'))
})
test('proven UART gets reset fallback only after no-reset fails', async () => {
  const f = fixture({ descriptors: [{ path: 'COM9', vendorId: 0x10c4, productId: 0xea60 }] })
  let count = 0; f.raw.initialize = async () => { if (++count === 1) throw new Error('sync timeout') }
  await f.loader.initialize()
  assert.deepEqual(f.events.filter(value => Array.isArray(value) && value[0] === 'loader').map(value => value[1].connectMode), ['no_reset', 'default_reset'])
  await f.loader.disconnect()
})
for (const descriptors of [[], [{ path: 'COM9', vendorId: 0x303a, productId: 0x20 }]]) test('unknown/OTG ports never receive UART reset fallback', async () => {
  const f = fixture({ descriptors, error: new Error('sync failed') })
  await assert.rejects(f.loader.initialize(), /sync failed/)
  assert.equal(f.events.filter(value => Array.isArray(value) && value[0] === 'loader').length, 1)
})
test('a bound ROM returning to runtime may recover once before writes', async () => {
  const selected = { port: 'COM9', expectedMac: '001122334455', transition: { requested: false, observed: 'rom' } }
  const f = fixture({ selected }), recovered = { ...selected, port: 'COM10', transition: { requested: true, observed: 'rom' } }
  let count = 0, recoveries = 0; f.raw.initialize = async () => { if (++count === 1) throw new Error('sync failed') }
  f.dependencies.recoverPort = async () => { ++recoveries; return recovered }
  await f.loader.initialize(); assert.equal(f.loader.port, 'COM10'); assert.equal(recoveries, 1)
  await f.loader.disconnect()
})
test('requested transitions cannot replay bootloader requests', async () => {
  const f = fixture({ selected: { port: 'COM9', expectedMac: '001122334455', transition: { requested: true, observed: 'rom' } }, error: new Error('sync') })
  f.dependencies.recoverPort = () => assert.fail('must not retry')
  await assert.rejects(f.loader.initialize(), /sync/)
})
test('abort interrupts initialization and closes the native port', async () => {
  const f = fixture(), controller = new AbortController(), reason = new Error('cancel')
  let started; const start = new Promise(resolve => { started = resolve })
  f.raw.initialize = () => { started(); return new Promise(() => {}) }
  const loader = openNodeRom({ port: 'COM9', signal: controller.signal }, f.dependencies)
  const pending = loader.initialize(); await start; controller.abort(reason)
  await assert.rejects(pending, error => error === reason); assert.ok(f.events.includes('close-port'))
  await assert.rejects(loader.runStub(), { code: 'ROM_CLOSED' })
})
test('concurrent ROM commands cannot interleave wire requests', async () => {
  const f = fixture(); await f.loader.initialize()
  let release; f.raw.getSecurityInfo = () => new Promise(resolve => { release = resolve })
  const pending = f.loader.getSecurityInfo(); await assert.rejects(f.loader.hardReset(), { code: 'ROM_BUSY' })
  release({ flags: 0, flashCryptCnt: 0 }); await pending; await f.loader.disconnect()
})

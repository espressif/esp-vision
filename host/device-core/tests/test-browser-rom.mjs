// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { openBrowserRom } from '../src/browser.js'
import { browserRomCompatibility } from '../src/browser_rom_compat.js'

const port = () => ({ open() { throw new Error('Port must remain passive') }, close() {} })
test('browser ROM construction and disconnect are passive without browser globals', async () => {
  const adapter = openBrowserRom({ port: port() })
  assert.equal(adapter.chipFamily, null)
  assert.equal(adapter.chipRevision, null)
  assert.equal(adapter.chipName, null)
  await adapter.disconnect(); await adapter.disconnect()
  await assert.rejects(adapter.initialize(), { code: 'ROM_CLOSED' })
})
test('browser ROM validates its port and cancellation before loading the browser dependency', async () => {
  assert.throws(() => openBrowserRom(), /SerialPort/)
  assert.throws(() => openBrowserRom({ port: port(), signal: {} }), /AbortSignal/)
  const controller = new AbortController()
  controller.abort(new Error('cancelled before open'))
  await assert.rejects(openBrowserRom({ port: port(), signal: controller.signal }).initialize(), /cancelled before open/)
})
test('SLIP prefix compatibility retries only pre-header bytes within the original deadline', async () => {
  let count = 0
  const loader = browserRomCompatibility({ async readPacket() {
    if (++count < 3) throw new Error('Invalid head of packet (0x47)')
    return [79, 72, 65, 73]
  } })
  assert.deepEqual(await loader.readPacket(100), [79, 72, 65, 73])
  loader.readPacket = browserRomCompatibility({ readPacket: async () => { throw new Error('Invalid SLIP escape (0xdb, 0x47)') } }).readPacket
  await assert.rejects(loader.readPacket(100), /Invalid SLIP escape/)
})
test('SLIP log noise cannot extend the operation deadline indefinitely', async () => {
  let count = 0
  const loader = browserRomCompatibility({ async readPacket() {
    count++
    await new Promise(resolve => setTimeout(resolve, 5))
    throw new Error('Invalid head of packet (0x47)')
  } })
  await assert.rejects(loader.readPacket(15), /Invalid head/)
  assert.ok(count <= 4)
})
function stubFixture({ native = true, error = 'Timed out waiting for packet header', handshake = true } = {}) {
  let timeout, reads = 0
  const stub = { IS_STUB: true, readPacket: async () => [], checkCommand: (...args) => { timeout = args[3] } }
  const raw = {
    isUsbJtagOrOtg: native,
    memFinish: async () => { if (error) throw new Error(error) },
    readPacket: async () => { reads++; return handshake ? [79, 72, 65, 73] : [] },
    async runStub(skip) {
      assert.equal(skip, true)
      await this.memFinish()
      assert.deepEqual(await this.readPacket(2500), [79, 72, 65, 73], 'OHAI required')
      return stub
    },
  }
  return { loader: browserRomCompatibility(raw), stub, timeout: () => timeout, reads: () => reads }
}
test('native USB may miss MEM_END acknowledgement but still requires OHAI', async () => {
  const fixture = stubFixture()
  assert.equal(await fixture.loader.runStub(true), fixture.stub)
  assert.equal(fixture.reads(), 1)
  await assert.rejects(stubFixture({ handshake: false }).loader.runStub(true), /OHAI required/)
})
test('UART timeouts and native USB non-timeout errors retain their failures', async () => {
  await assert.rejects(stubFixture({ native: false }).loader.runStub(true), /Timed out/)
  await assert.rejects(stubFixture({ error: 'Port disconnected' }).loader.runStub(true), /Port disconnected/)
})
test('stub launch restores temporary hooks on failure and rejects ROM fallback', async () => {
  const fixture = stubFixture({ handshake: false })
  const finish = fixture.loader.memFinish, packet = fixture.loader.readPacket
  await assert.rejects(fixture.loader.runStub(true))
  assert.equal(fixture.loader.memFinish, finish)
  assert.equal(fixture.loader.readPacket, packet)
  await assert.rejects(browserRomCompatibility({ runStub: async () => ({ IS_STUB: false }) }).runStub(true), /no supported browser flash stub/)
})
test('stub FLASH_BEGIN timeout scales with uncompressed erase size and preserves other commands', async () => {
  const fixture = stubFixture()
  const stub = await fixture.loader.runStub(true)
  await stub.checkCommand(2, [0, 0, 128, 0], 0, 3000)
  assert.ok(fixture.timeout() > 240000)
  await stub.checkCommand(3, [], 0, 1234)
  assert.equal(fixture.timeout(), 1234)
})

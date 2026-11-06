// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { openBrowserRom } from '../src/browser.js'
import { createOfficialBrowserLoader, browserRomInterface, ROM_UART_BAUD_RATES } from '../src/browser_rom.js'
import { browserTransportCompatibility, browserRomCompatibility, readBrowserFlash, romWords } from '../src/browser_rom_compat.js'
import { romMd5 } from '../src/rom_md5.js'
import { ESPLoader } from 'esptool-js'
import { ESP32S3ROM } from 'esptool-js/lib/targets/esp32s3.js'
import { ESP32P4ROM } from 'esptool-js/lib/targets/esp32p4.js'
import { ESP32S31ROM } from 'esptool-js/lib/targets/esp32s31.js'

const passivePort = () => ({ open() { throw new Error('Port must remain passive') }, close() {} })
test('browser ROM construction and disconnect remain passive without browser globals', async () => {
  const adapter = openBrowserRom({ port: passivePort() })
  assert.equal(adapter.chipFamily, null)
  assert.equal(adapter.chipRevision, null)
  assert.equal(adapter.chipName, null)
  assert.equal(adapter.chipMac, null)
  await adapter.disconnect(); await adapter.disconnect()
  await assert.rejects(adapter.initialize(), { code: 'ROM_CLOSED' })
})
test('browser ROM validates its port and cancellation before loading the dependency', async () => {
  assert.throws(() => openBrowserRom(), /SerialPort/)
  assert.throws(() => openBrowserRom({ port: passivePort(), signal: {} }), /AbortSignal/)
  const controller = new AbortController()
  controller.abort(new Error('cancelled before open'))
  await assert.rejects(openBrowserRom({ port: passivePort(), signal: controller.signal }).initialize(), /cancelled before open/)
})
test('expectedMac is validated synchronously and construction does not mutate options or ports', async () => {
  for (const expectedMac of [null, true, 1, {}, [], '', '30eda0eda1b', '30eda0eda1b00', '30:ed:a0:ed:a1:b0', '30eda0eda1bg', ' 30eda0eda1b0', '30eda0eda1b0\n']) {
    let touched = 0
    const port = Object.freeze({ getInfo() { touched++; return {} }, open() { touched++ }, close() { touched++ } })
    const options = Object.freeze({ port, expectedMac })
    assert.throws(() => openBrowserRom(options), /expectedMac/)
    assert.throws(() => createOfficialBrowserLoader({}, port, {}, { expectedMac }), /expectedMac/)
    assert.equal(touched, 0)
    assert.equal(options.expectedMac, expectedMac)
  }
  const port = Object.freeze(passivePort()), options = Object.freeze({ port, expectedMac: '30EDA0EDA1B0' })
  const adapter = openBrowserRom(options)
  assert.equal(adapter.chipMac, null)
  assert.equal(options.expectedMac, '30EDA0EDA1B0')
  await adapter.disconnect()
})
test('interface discovery separates known bridge, OTG and JTAG; custom descriptors need evidence', () => {
  const port = (usbVendorId, usbProductId) => ({ getInfo: () => ({ usbVendorId, usbProductId }) })
  assert.equal(browserRomInterface(port(0x10c4, 0xea60)), 'uart')
  assert.equal(browserRomInterface(port(0x303a, 32)), 'usb-otg')
  assert.equal(browserRomInterface(port(0x303a, 0x1001)), 'usb-jtag')
  assert.equal(browserRomInterface(port(0x303a, 0x1234)), 'unknown')
  assert.equal(browserRomInterface(port(undefined, undefined)), 'unknown')
  assert.equal(browserRomInterface(port(1, 2), 'uart'), 'uart')
  assert.throws(() => browserRomInterface(port(1, 2), 'usb'), /interface/)
})
test('ROM transport MD5 matches independent known vectors and multi-block data', () => {
  for (const input of ['', 'a', 'abc', 'message digest', 'a'.repeat(55), 'b'.repeat(56), 'c'.repeat(65), 'x'.repeat(65536)]) {
    const bytes = new TextEncoder().encode(input)
    assert.equal(Buffer.from(romMd5(bytes)).toString('hex'), createHash('md5').update(bytes).digest('hex'))
  }
})
function transportFixture() {
  return browserTransportCompatibility({ device: { readable: {}, writable: {} }, buffer: new Uint8Array(0),
    appendArray(a, b) { return new Uint8Array([...a, ...b]) }, async write() {},
  })
}
test('SLIP keeps a valid packet after boot noise, trailing packets and escaped bytes', async () => {
  const transport = transportFixture()
  transport.buffer = new Uint8Array([71, 97, 114, 0xc0, 0xc0, 79, 72, 65, 73, 0xc0, 0xc0, 0xdb, 0xdc, 0xdb, 0xdd, 0xc0])
  assert.deepEqual(await transport.read(100), new Uint8Array([79, 72, 65, 73]))
  assert.deepEqual(await transport.read(100), new Uint8Array([0xc0, 0xdb]))
  transport.buffer = new Uint8Array([0xc0, 0xdb, 0x47, 0xc0])
  await assert.rejects(transport.read(100), { code: 'ROM_SLIP_ESCAPE' })
})
test('SLIP preserves state across partial incoming chunks and has a total packet deadline', async () => {
  const transport = transportFixture()
  transport.buffer = new Uint8Array([0xc0, 1, 0xdb])
  const later = setTimeout(() => { transport.buffer = new Uint8Array([0xdc, 2, 0xc0]) }, 5)
  try { assert.deepEqual(await transport.read(100), new Uint8Array([1, 0xc0, 2])) } finally { clearTimeout(later) }
  transport.buffer = new Uint8Array([0xc0, 1])
  await assert.rejects(transport.read(15), { code: 'ROM_READ_TIMEOUT' })
  transport.device.readable = null
  await assert.rejects(transport.read(100), { code: 'ROM_PORT_LOST' })
  transport.device.writable = null
  await assert.rejects(transport.write(new Uint8Array()), { code: 'ROM_PORT_LOST' })
})
function readFixture({ data = new Uint8Array([1, 2, 3]), packets, digest = romMd5(data), delay = 0 } = {}) {
  const received = [], reads = [...(packets ?? [data]), digest]
  const loader = { IS_STUB: true, ESP_READ_FLASH: 0xd2, transport: { baudrate: 2000000,
    async read() { if (delay) await new Promise(resolve => setTimeout(resolve, delay)); const packet = reads.shift(); if (!packet) throw new Error('missing packet'); return packet },
    async write(bytes) { received.push(new DataView(bytes.buffer).getUint32(0, true)) },
  }, async checkCommand(description, opcode, payload) { assert.equal(description, 'read flash'); assert.equal(opcode, 0xd2); assert.deepEqual(payload, romWords(0x20000, data.length, 1024, 1)); return 0 } }
  return { loader, data, received, reads }
}
test('readback ACKs partial packets and consumes the delayed trailing digest before returning', async () => {
  const f = readFixture({ packets: [new Uint8Array([1]), new Uint8Array([2, 3])], delay: 3 })
  assert.deepEqual(await readBrowserFlash(f.loader, 0x20000, 3), f.data)
  assert.deepEqual(f.received, [1, 3]); assert.equal(f.reads.length, 0)
})
test('readback rejects malformed packets, incomplete data, incorrect or absent digest', async () => {
  for (const config of [{ packets: [new Uint8Array(0)] }, { packets: [new Uint8Array([1, 2, 3, 4])] }, { digest: new Uint8Array(16) }, { digest: new Uint8Array(15) }]) {
    const f = readFixture(config)
    await assert.rejects(readBrowserFlash(f.loader, 0x20000, 3), /packet|digest/i)
  }
  const f = readFixture(); f.loader.transport.read = async () => { throw Object.assign(new Error('timeout waiting for trailing digest'), { code: 'ROM_READ_TIMEOUT' }) }
  await assert.rejects(readBrowserFlash(f.loader, 0x20000, 3), { code: 'ROM_READ_TIMEOUT' })
})
test('native USB MEM_END timeout exception never bypasses validated stub greeting', async () => {
  const native = browserRomCompatibility({ memFinish: async () => { throw new Error('Timed out receiving a complete ROM packet') } }, { nativeUsb: true })
  await native.memFinish()
  const uart = browserRomCompatibility({ memFinish: async () => { throw new Error('Timed out') } })
  await assert.rejects(uart.memFinish(), /Timed out/)
  const lost = browserRomCompatibility({ memFinish: async () => { throw new Error('Port disconnected') } }, { nativeUsb: true })
  await assert.rejects(lost.memFinish(), /disconnected/)
})
function officialFixture({ failRates = [], usb = false, security = { flags: 0, flashCryptCnt: 0 }, failWrite = false, recoverSecurity, recoverFamily, detach = false, reopenResets = false, flashId = 0x1840ef, stubStarts = true, signal, expectedMac, mac = '30:ed:a0:ed:a1:b0', recoverMac } = {}) {
  const events = [], calls = [], loaders = [], opens = [], reads = []
  const port = { readable: null, writable: null,
    getInfo: () => ({ usbVendorId: usb ? 0x303a : 0x10c4, usbProductId: usb ? 32 : 0xea60 }),
    async open({ baudRate }) { opens.push(baudRate); this.readable = {}; this.writable = {} },
    async close() { this.readable = null; this.writable = null },
  }
  class FakeTransport {
    constructor(device) { this.device = device; this.buffer = new Uint8Array(0); this.baudrate = 115200 }
    appendArray(a, b) { return new Uint8Array([...a, ...b]) }
    flushInput() { this.buffer = new Uint8Array(0) }
    async disconnect() { calls.push('close'); await port.close() }
    async changeBaudrate(rate) { calls.push(['retune', rate]); this.baudrate = rate; if (detach) throw Object.assign(new Error('Port detached'), { code: 'ROM_PORT_LOST' }); await port.close(); await port.open({ baudRate: rate }); if (reopenResets) loaders.at(-1).IS_STUB = false }
    async write() {}
    async setDTR(value) { calls.push(['dtr', value]) }
    async setRTS(value) { calls.push(['rts', value]) }
  }
  class FakeLoader {
    constructor({ transport }) { this.transport = transport; this.CHIP_DETECT_MAGIC_REG_ADDR = 0x40001000; this.ESP_CHANGE_BAUDRATE = 15; this.ESP_READ_FLASH = 0xd2; this.ESP_ERASE_REGION = 0xd1; this.ERASE_REGION_TIMEOUT_PER_MB = 30000; this.chip = { CHIP_NAME: loaders.length && recoverFamily ? recoverFamily : 'ESP32-S31', getChipRevision: async () => 0, readMac: async reader => { assert.equal(reader, this); calls.push('mac'); return loaders.length > 1 && recoverMac !== undefined ? recoverMac : mac }, postConnect: async () => calls.push('postConnect') }; loaders.push(this) }
    async detectChip(mode) { calls.push(['detect', mode]); await port.open({ baudRate: 115200 }); this.transport.baudrate = 115200 }
    async getSecurityInfo() { return loaders.length > 1 && recoverSecurity ? recoverSecurity : security }
    async runStub() { calls.push('stub'); this.IS_STUB = stubStarts }
    async readReg() { if (!this.IS_STUB || failRates.includes(this.transport.baudrate)) throw new Error('speed probe timeout'); return 123 }
    async readFlashId() { return flashId }
    async writeReg(...args) { calls.push(['register-write', ...args]) }
    timeoutPerMb(...args) { return ESPLoader.prototype.timeoutPerMb.apply(this, args) }
    async checkCommand(description, _opcode, request) {
      if (description === 'change baud rate') { calls.push(['change', new DataView(request.buffer).getUint32(0, true)]); return 0 }
      if (description === 'erase region') { calls.push(['erase', ...new Uint32Array(request.buffer)]); return 0 }
      const length = new DataView(request.buffer).getUint32(4, true), data = new Uint8Array(length)
      const frame = bytes => new Uint8Array([0xc0, ...bytes, 0xc0])
      // All-zero bytes and their digest need no SLIP escaping in this fixture.
      const payload = []
      for (let offset = 0; offset < length; offset += 1024) payload.push(...frame(data.slice(offset, offset + 1024)))
      const md5 = romMd5(data)
      const escaped = []
      for (const byte of md5) { if (byte === 0xc0) escaped.push(0xdb, 0xdc); else if (byte === 0xdb) escaped.push(0xdb, 0xdd); else escaped.push(byte) }
      payload.push(0xc0, ...escaped, 0xc0); this.transport.buffer = new Uint8Array(payload); reads.push(length)
      return 0
    }
    async writeFlash(options) { calls.push(['write', options.compress, options.eraseAll, options.fileArray[0].address]); if (failWrite) throw new Error('write interrupted'); options.calculateMD5Hash(options.fileArray[0].data); options.reportProgress(0, 10, 10) }
  }
  const adapter = createOfficialBrowserLoader({ ESPLoader: FakeLoader, Transport: FakeTransport }, port, {}, { onStatus: event => events.push(event), signal, expectedMac })
  return { adapter, events, calls, port, loaders, opens, reads }
}
test('the official P4/S3/S31 factory MAC readers supply the normalized public chipMac', async () => {
  for (const Chip of [ESP32P4ROM, ESP32S3ROM, ESP32S31ROM]) {
    const chip = new Chip(), registers = []
    const mac = await chip.readMac({ async readReg(address) {
      registers.push(address)
      return address === chip.MAC_EFUSE_REG ? 0xa0eda1b0 : 0x30ed
    } })
    assert.equal(mac, '30:ed:a0:ed:a1:b0')
    assert.deepEqual(registers, [chip.MAC_EFUSE_REG, chip.MAC_EFUSE_REG + 4])
    const f = officialFixture({ usb: true, mac, expectedMac: '30EDA0EDA1B0' })
    assert.equal(f.adapter.chipMac, null)
    await f.adapter.initialize()
    assert.equal(f.adapter.chipMac, '30eda0eda1b0')
    assert.ok(!f.calls.includes('stub'))
    await f.adapter.disconnect()
  }
})
test('a first expected MAC mismatch keeps the identity error on USB and UART and permanently closes', async () => {
  for (const usb of [true, false]) {
    const f = officialFixture({ usb, expectedMac: '30eda0eda1b1' })
    await assert.rejects(f.adapter.initialize(), { code: 'ROM_IDENTITY_CHANGED' })
    assert.equal(f.adapter.chipMac, null)
    assert.equal(f.port.readable, null)
    assert.equal(f.calls.filter(call => call === 'close').length, 1)
    assert.ok(!f.calls.includes('stub'))
    assert.ok(!f.calls.some(call => Array.isArray(call) && ['write', 'erase', 'register-write'].includes(call[0])))
    await assert.rejects(f.adapter.initialize(), { code: 'ROM_CLOSED' })
    await assert.rejects(f.adapter.runStub(), { code: 'ROM_CLOSED' })
    assert.equal(f.loaders.length, 1)
  }
})
test('a recovered same-family/revision device with a different MAC never receives another stub or write', async () => {
  for (const expectedMac of [undefined, '30eda0eda1b0']) {
    const f = officialFixture({ expectedMac, failRates: [2000000], recoverMac: '30:ed:a0:ed:a1:b1' })
    await f.adapter.initialize()
    await assert.rejects(f.adapter.runStub(), { code: 'ROM_IDENTITY_CHANGED' })
    assert.equal(f.adapter.chipMac, '30eda0eda1b0')
    assert.equal(f.calls.filter(call => call === 'stub').length, 1)
    assert.equal(f.loaders.length, 2)
    assert.equal(f.port.readable, null)
    assert.ok(!f.calls.some(call => Array.isArray(call) && ['write', 'erase'].includes(call[0])))
    await assert.rejects(f.adapter.runStub(), { code: 'ROM_CLOSED' })
  }
})
test('malformed official MAC replies fail closed before stub startup on native USB', async () => {
  for (const mac of [null, false, [], new Uint8Array(6), '', '30eda0eda1b0', '30:ed:a0:ed:a1', '30:ed:a0:ed:a1:gg', '30:ed:a0:ed:a1:b0\n']) {
    const f = officialFixture({ usb: true, mac })
    await assert.rejects(f.adapter.initialize(), { code: 'ROM_IDENTITY_INVALID' })
    assert.equal(f.adapter.chipMac, null)
    assert.equal(f.port.readable, null)
    assert.ok(!f.calls.includes('stub'))
    await assert.rejects(f.adapter.runStub(), { code: 'ROM_CLOSED' })
  }
})
test('an invalid recovery MAC stops baud fallback without opening another port or starting another stub', async () => {
  const f = officialFixture({ failRates: [2000000], recoverMac: '' })
  await f.adapter.initialize()
  await assert.rejects(f.adapter.runStub(), { code: 'ROM_IDENTITY_INVALID' })
  assert.equal(f.loaders.length, 2)
  assert.equal(f.calls.filter(call => call === 'stub').length, 1)
  assert.equal(f.port.readable, null)
  assert.ok(!f.calls.some(call => Array.isArray(call) && ['write', 'erase'].includes(call[0])))
  await assert.rejects(f.adapter.initialize(), { code: 'ROM_CLOSED' })
})
test('ROM UART starts at 115200, checks candidates in order and stops at the first success', async () => {
  const f = officialFixture({ failRates: [2000000, 1500000] })
  await f.adapter.initialize(); assert.equal(f.adapter.chipFamily, 0x3231)
  const stub = await f.adapter.runStub(true)
  assert.equal(stub.IS_STUB, true)
  assert.equal(f.adapter.baudRate, 921600)
  assert.deepEqual(f.events.filter(event => event.phase === 'baud-testing').map(event => event.baudRate), ROM_UART_BAUD_RATES.slice(0, 3))
  assert.equal(f.loaders.length, 3, 'failed candidates must have fresh synchronized, qualified stubs')
  assert.deepEqual(f.opens, [115200, 2000000, 115200, 1500000, 115200, 921600])
  await stub.flashData(new ArrayBuffer(4), () => {}, 0x10000, false)
  assert.deepEqual(f.calls.find(call => Array.isArray(call) && call[0] === 'write'), ['write', true, false, 0x10000])
  await f.adapter.disconnect()
})
test('all high rates fail safely back to validated 115200', async () => {
  const f = officialFixture({ failRates: ROM_UART_BAUD_RATES.slice(0, -1) })
  await f.adapter.initialize(); await f.adapter.runStub()
  assert.equal(f.adapter.baudRate, 115200)
  assert.deepEqual(f.events.filter(event => event.phase === 'baud-testing').map(event => event.baudRate), ROM_UART_BAUD_RATES)
})

test('browser official write enables device MD5 and returns an exact verified range receipt', async () => {
  const f = officialFixture({ usb: true })
  await f.adapter.initialize()
  const stub = await f.adapter.runStub()
  const bytes = new Uint8Array([0xe9, 1, 2, 3])
  const result = await stub.flashData(bytes.buffer, () => {}, 0x10000)
  assert.deepEqual(result, { algorithm: 'md5', offset: 0x10000, bytes: 4,
    digest: createHash('md5').update(bytes).digest('hex') })
  await f.adapter.disconnect()
})
test('port reopen that resets the board does not count as an initialized high-speed stub', async () => {
  const f = officialFixture({ reopenResets: true })
  await f.adapter.initialize(); await f.adapter.runStub()
  assert.equal(f.adapter.baudRate, 115200)
  assert.equal(f.loaders.length, 5)
})
test('native USB connects directly to ready ROM without UART rate or signal changes', async () => {
  const f = officialFixture({ usb: true })
  await f.adapter.initialize(); await f.adapter.runStub()
  assert.deepEqual(f.calls[0], ['detect', 'no_reset'])
  assert.equal(f.adapter.interface, 'usb-otg'); assert.equal(f.adapter.baudRate, null)
  assert.ok(!f.calls.some(call => Array.isArray(call) && ['retune', 'rts', 'dtr'].includes(call[0])))
})
test('recovery rechecks chip identity and security before another stub or write', async () => {
  for (const setting of [{ recoverSecurity: { flags: 1, flashCryptCnt: 0 }, code: 'ROM_SECURITY' }, { recoverFamily: 'ESP32-S3', code: 'ROM_IDENTITY_CHANGED' }]) {
    const f = officialFixture({ failRates: [2000000], ...setting })
    await f.adapter.initialize()
    await assert.rejects(f.adapter.runStub(), { code: setting.code })
    assert.equal(f.calls.filter(call => call === 'stub').length, 1)
    assert.ok(!f.calls.some(call => Array.isArray(call) && call[0] === 'write'))
  }
})
test('write interruption invalidates the session and never retries or resumes compressed data', async () => {
  const f = officialFixture({ failWrite: true })
  await f.adapter.initialize(); const stub = await f.adapter.runStub()
  await assert.rejects(stub.flashData(new ArrayBuffer(4), () => {}, 0, true), /write interrupted/)
  await assert.rejects(stub.readFlash(0, 4), /write interrupted/)
  assert.equal(f.calls.filter(call => Array.isArray(call) && call[0] === 'write').length, 1)
  assert.equal(f.port.readable, null)
})
test('detached download port is reported instead of blind fallback using a stale handle', async () => {
  const f = officialFixture({ detach: true })
  await f.adapter.initialize()
  await assert.rejects(f.adapter.runStub(), { code: 'ROM_PORT_LOST' })
  assert.equal(f.loaders.length, 1)
})

test('S31 native USB reset uses the watchdog without stale-port control signals', async () => {
  const f = officialFixture({ usb: true })
  await f.adapter.initialize(); await f.adapter.runStub(); await f.adapter.hardReset()
  assert.deepEqual(f.calls.filter(call => Array.isArray(call) && call[0] === 'register-write'), [
    ['register-write', 0x20801018, 0x50d83aa1], ['register-write', 0x20801004, 2000], ['register-write', 0x20801000, 0xd0000102], ['register-write', 0x20801018, 0],
  ])
  assert.ok(!f.calls.some(call => Array.isArray(call) && ['rts', 'dtr'].includes(call[0])))
})

test('SPI RDID normalizes only the documented24bits while rejecting non-register values', async () => {
  for (const raw of [-15187898, 0xff184046]) {
    const f = officialFixture({ usb: true, flashId: raw })
    await f.adapter.initialize(); const stub = await f.adapter.runStub()
    assert.equal(await stub.flashId(), 0x184046)
  }
  const f = officialFixture({ usb: true, flashId: NaN })
  await f.adapter.initialize(); const stub = await f.adapter.runStub()
  await assert.rejects(stub.flashId(), { code: 'ROM_FLASH_ID' })
})

test('missing native stub handshake never creates a usable stub or starts a write', async () => {
  const f = officialFixture({ usb: true, stubStarts: false })
  await f.adapter.initialize()
  await assert.rejects(f.adapter.runStub(), { code: 'ROM_STUB_REQUIRED' })
  assert.ok(!f.calls.some(call => Array.isArray(call) && call[0] === 'write'))
  assert.equal(f.port.readable, null)
})

test('region erase uses the real official checked command and size-scaled timeout without chip erase', async () => {
  const f = officialFixture({ usb: true })
  await f.adapter.initialize(); const stub = await f.adapter.runStub()
  const requests = [], upstream = f.loaders[0]
  upstream.debug = () => {}
  upstream.checkCommand = ESPLoader.prototype.checkCommand
  upstream.command = async (...args) => { requests.push(args); return [0, new Uint8Array([0, 0])] }
  await stub.eraseRegion(0x710000, 0x8f0000)
  await stub.eraseRegion(0xfffff000, 4096)
  assert.deepEqual(requests, [[0xd1, romWords(0x710000, 0x8f0000), 0, undefined,
    upstream.timeoutPerMb(30000, 0x8f0000)], [0xd1, romWords(0xfffff000, 4096), 0, undefined, 3000]])
  assert.deepEqual(f.events.at(-1), { phase: 'erasing-region', address: 0xfffff000, size: 4096 })
  assert.ok(!f.calls.some(call => Array.isArray(call) && call[0] === 'write'))
  await f.adapter.disconnect()
})

test('region erase rejects malformed ranges before sending an erase command', async () => {
  for (const [address, size] of [[-4096, 4096], [1, 4096], [0, 0], [0, 1], [0, 0x100000000],
    [0xfffff000, 8192], [NaN, 4096], [true, 4096], [0, true], [0x100000000, 4096]]) {
    const f = officialFixture({ usb: true })
    await f.adapter.initialize(); const stub = await f.adapter.runStub()
    await assert.rejects(stub.eraseRegion(address, size), RangeError)
    assert.ok(!f.calls.some(call => Array.isArray(call) && call[0] === 'erase'))
    await f.adapter.disconnect()
  }
})

test('region erase requires a live validated stub and supported upstream command primitives', async () => {
  for (const invalid of ['stub', 'command', 'timeout', 'budget']) {
    const f = officialFixture({ usb: true })
    await f.adapter.initialize(); const stub = await f.adapter.runStub()
    if (invalid === 'stub') f.loaders[0].IS_STUB = false
    if (invalid === 'command') f.loaders[0].ESP_ERASE_REGION = 0xd0
    if (invalid === 'timeout') f.loaders[0].timeoutPerMb = null
    if (invalid === 'budget') f.loaders[0].ERASE_REGION_TIMEOUT_PER_MB = NaN
    await assert.rejects(stub.eraseRegion(0x710000, 4096), { code: invalid === 'stub' ? 'ROM_STUB_REQUIRED' : 'ROM_ERASE_UNSUPPORTED' })
    assert.ok(!f.calls.some(call => Array.isArray(call) && call[0] === 'erase'))
    await f.adapter.disconnect()
  }
})

test('real upstream region erase status, truncated replies and operation errors invalidate the session without retry', async () => {
  for (const reply of [[0, new Uint8Array([1, 0])], [0, new Uint8Array([0])], [1, new Uint8Array([0, 0])], new Error('erase interrupted')]) {
    const f = officialFixture({ usb: true })
    await f.adapter.initialize(); const stub = await f.adapter.runStub()
    let commands = 0
    f.loaders[0].debug = () => {}
    f.loaders[0].checkCommand = ESPLoader.prototype.checkCommand
    f.loaders[0].command = async () => { commands++; if (reply instanceof Error) throw reply; return reply }
    await assert.rejects(stub.eraseRegion(0x710000, 4096))
    await assert.rejects(stub.flashData(new ArrayBuffer(4), () => {}, 0))
    await assert.rejects(stub.eraseRegion(0x710000, 4096))
    assert.equal(commands, 1)
    assert.equal(f.port.readable, null)
  }
})

test('region erase cancellation closes a pending request and never reports completion or retries', async () => {
  const controller = new AbortController(), f = officialFixture({ usb: true, signal: controller.signal })
  await f.adapter.initialize(); const stub = await f.adapter.runStub()
  let started, commands = 0
  f.loaders[0].checkCommand = () => new Promise(resolve => { commands++; started = resolve })
  const erasing = stub.eraseRegion(0x710000, 4096), failed = assert.rejects(erasing, { name: 'AbortError' })
  controller.abort(); await failed
  started(0)
  await assert.rejects(stub.flashData(new ArrayBuffer(4), () => {}, 0))
  assert.equal(commands, 1); assert.equal(f.port.readable, null)
})

test('a detached port cannot report region erase success even after a stale successful reply', async () => {
  const f = officialFixture({ usb: true })
  await f.adapter.initialize(); const stub = await f.adapter.runStub()
  let commands = 0
  f.loaders[0].checkCommand = async () => { commands++; await f.port.close(); return 0 }
  await assert.rejects(stub.eraseRegion(0x710000, 4096), { code: 'ROM_PORT_LOST' })
  await assert.rejects(stub.eraseRegion(0x710000, 4096))
  assert.equal(commands, 1)
})

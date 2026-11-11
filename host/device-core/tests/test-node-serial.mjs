// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { setImmediate as turn } from 'node:timers/promises'
import { serialTransport } from '../src/node_transport.js'
import { NodeSerialPort, serialPortInfo, serialFailure } from '../src/node_serial.js'
import { serialFixture } from './serial-fixture.mjs'

test('native Windows and Unix permission errors retain port-specific diagnosis', () => {
  for (const message of ['Opening COM9: Access denied', 'Permission denied, cannot open /dev/ttyACM0']) {
    assert.equal(serialFailure(new Error(message), 'open').code, 'SERIAL_ACCESS_DENIED')
  }
})

test('native runtime transport is passive, exclusive and keeps byte/control ordering', async () => {
  const f = serialFixture(), received = []
  const transport = serialTransport({ serial: 'COM9' }, { binding: f.binding })
  transport.subscribe({ data: bytes => received.push(...bytes) })
  assert.deepEqual(f.events, [])
  await transport.connect()
  assert.equal(f.events[0][1].lock, true)
  assert.equal(f.events[0][1].hupcl, false)
  assert.deepEqual(f.events[1], ['signals', { dtr: true, rts: false }])
  f.push(Uint8Array.of(0, 0xc0, 255)); await turn()
  assert.deepEqual(received, [0, 0xc0, 255])
  const data = Uint8Array.of(4, 5)
  const write = transport.writeBytes(data); data.fill(9)
  await Promise.all([write, transport.drain(), transport.setBaudRate(921600)])
  assert.deepEqual(f.events.slice(2), [['write', [4, 5]], ['drain'], ['baud', { baudRate: 921600 }]])
  assert.equal(transport.getDiagnostics().baudRate, 921600)
  await transport.disconnect()
  assert.equal(f.handle.isOpen, false)
  await assert.rejects(transport.writeBytes(Uint8Array.of(1)), { code: 'SERIAL_CLOSED' })
})

test('Web Serial-shaped npm port preserves the other modem signals and changes baud in place', async () => {
  const f = serialFixture(), port = new NodeSerialPort({ path: 'COM3', vendorId: 0x303a, productId: 0x1001 }, f)
  assert.deepEqual(port.getInfo(), { usbVendorId: 0x303a, usbProductId: 0x1001 })
  await port.open({ baudRate: 115200 })
  assert.deepEqual(f.events[1], ['signals', { dtr: false, rts: false }])
  await port.setSignals({ dataTerminalReady: true })
  await port.setSignals({ requestToSend: true })
  assert.deepEqual(f.events.at(-1), ['signals', { dtr: true, rts: true }])
  await port.setBaudRate(460800)
  assert.equal(f.events.filter(([kind]) => kind === 'open').length, 1)
  const reader = port.readable.getReader()
  const pending = reader.read()
  await reader.cancel()
  assert.equal((await pending).done, true)
  reader.releaseLock()
  assert.equal(port.readable, null)
  assert.equal(f.handle.isOpen, false)
  await port.close()
})

test('late writes and queued bytes from an old connection cannot reach or close a replacement', async () => {
  const first = serialFixture(), second = serialFixture()
  let opens = 0, started, rejectWrite
  const writing = new Promise(resolve => { started = resolve })
  first.handle.write = () => new Promise((_, reject) => { rejectWrite = reject; started() })
  const transport = serialTransport({ serial: 'COM9' }, { binding: () => (++opens === 1 ? first : second).binding() })
  await transport.connect()
  const old = assert.rejects(transport.writeBytes(Uint8Array.of(1)), { code: 'SERIAL_CLOSED' })
  await writing
  const queued = assert.rejects(transport.writeBytes(Uint8Array.of(2)), { code: 'SERIAL_CLOSED' })
  const closing = transport.disconnect()
  await assert.rejects(transport.connect(), { code: 'TRANSPORT_BUSY' })
  await closing; await transport.connect()
  rejectWrite(new Error('old descriptor closed'))
  await old; await queued
  assert.equal(transport.getDiagnostics().connected, true)
  await transport.writeBytes(Uint8Array.of(3))
  assert.deepEqual(second.events.filter(([kind]) => kind === 'write'), [['write', [3]]])
  await transport.disconnect()
})

for (const [code, expected] of [['EACCES', 'SERIAL_ACCESS_DENIED'], ['EPERM', 'SERIAL_ACCESS_DENIED'], ['EBUSY', 'SERIAL_BUSY']]) {
  test(`native open maps ${code} without a Python diagnostic subprocess`, async () => {
    const transport = serialTransport({ serial: 'COM3' }, { binding: async () => ({ open: async () => { throw Object.assign(new Error('private'), { code }) } }) })
    await assert.rejects(transport.connect(), error => error.code === expected && !error.message.includes('private'))
    await transport.disconnect()
  })
}

test('cancel during native open closes a late handle without reading or writing', async () => {
  const f = serialFixture(), controller = new AbortController(), reason = new Error('cancelled')
  let resolveOpen
  const transport = serialTransport({ serial: 'COM9' }, { binding: async () => ({ open: () => new Promise(resolve => { resolveOpen = resolve }) }) })
  const opening = transport.connect({ signal: controller.signal })
  await turn(); controller.abort(reason)
  await assert.rejects(opening)
  resolveOpen(f.handle); await turn()
  assert.equal(f.handle.isOpen, false)
  assert.deepEqual(f.events, [['close']])
})

test('unplug fails pending reader, emits one failure and closes the handle', async () => {
  const f = serialFixture(), events = [], transport = serialTransport({ serial: 'COM9' }, f)
  transport.subscribe({ error: error => events.push(error.code), close: () => events.push('close') })
  await transport.connect()
  f.push(Object.assign(new Error('device removed'), { code: 'EIO' }))
  await turn()
  assert.deepEqual(events, ['SERIAL_ERROR', 'close'])
  assert.equal(transport.getDiagnostics().lastError.stage, 'read')
  await transport.disconnect()
})

test('signal configuration failure closes an acquired native port', async () => {
  const f = serialFixture(); f.handle.set = async () => { throw new Error('unsupported') }
  const port = new NodeSerialPort({ path: 'COM9' }, f)
  await assert.rejects(port.open(), /unsupported/)
  assert.equal(f.handle.isOpen, false)
})

test('Linux enumeration recovers physical USB topology and runtime product without opening', async () => {
  const directory = '/sys/devices/pci/usb1/1-2'
  const descriptors = { idVendor: '303a', idProduct: '4003', serial: '001122334455', product: 'ESP-VISION' }
  const info = await serialPortInfo({ path: '/dev/ttyACM0' }, {
    platform: 'linux', resolvePath: async () => directory + '/1-2:1.0',
    read: async path => { const name = path.slice(directory.length + 1); if (!(name in descriptors)) throw new Error('not here'); return descriptors[name] },
  })
  assert.equal(info.location, '1-2')
  assert.equal(info.productId, 0x4003)
  assert.equal(info.serialNumber, descriptors.serial)
  assert.equal(info.product, 'ESP-VISION')
})

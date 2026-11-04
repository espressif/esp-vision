// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { serialTransport } from '../src/browser.js'

class Port extends EventTarget {
  opens = []
  openOptions = []
  closes = 0
  writes = []
  lines = []
  rejected = []
  async open(options) {
    const { baudRate } = options
    this.openOptions.push(options)
    this.opens.push(baudRate)
    if (this.rejected.includes(baudRate)) throw new Error('unsupported speed')
    assert.ok(!this.readable && !this.writable)
    this.readable = new ReadableStream({ start: controller => { this.input = controller } })
    this.writable = new WritableStream({ write: async bytes => {
      if (this.writeGate) await this.writeGate
      this.writes.push(new Uint8Array(bytes))
    } })
  }
  async close() {
    assert.ok(!this.readable?.locked, 'read lock must be released before SerialPort.close')
    assert.ok(!this.writable?.locked, 'write lock must be released before SerialPort.close')
    this.readable = this.writable = null
    this.closes++
  }
  async setSignals(signals) { this.lines.push(signals) }
  forget() { throw new Error('Internal retune must preserve browser authorization') }
}

test('browser runtime adapter is passive and reopens its authorized port without disconnect events', async () => {
  const port = new Port(), transport = serialTransport({ port }), data = [], events = []
  assert.deepEqual(port.opens, [])
  const unsubscribe = transport.subscribe({ data: bytes => data.push(...bytes), close: () => events.push('close'), error: error => events.push(error.code) })
  await transport.connect()
  assert.deepEqual(port.opens, [115200])
  port.input.enqueue(new Uint8Array([1, 2]))
  await transport.setBaudRate(2000000)
  port.input.enqueue(new Uint8Array([3, 4]))
  await transport.writeBytes(new Uint8Array([5, 6]))
  assert.deepEqual(data, [1, 2, 3, 4])
  assert.deepEqual(port.opens, [115200, 2000000])
  assert.deepEqual(port.openOptions, [{ baudRate: 115200, bufferSize: 256 * 1024 }, { baudRate: 2000000, bufferSize: 256 * 1024 }])
  assert.equal(port.closes, 1)
  assert.deepEqual(events, [])
  assert.deepEqual(transport.getDiagnostics(), { baudRate: 2000000, connected: true, reopening: false })
  assert.deepEqual(port.lines, [{ dataTerminalReady: true, requestToSend: false }, { dataTerminalReady: true, requestToSend: false }])
  await transport.disconnect()
  await transport.disconnect()
  assert.equal(port.closes, 2)
  assert.deepEqual(events, [])
  unsubscribe()
})

test('browser retune waits for writes to finish and a rejected rate can reopen baseline', async () => {
  const port = new Port(), transport = serialTransport({ port })
  transport.subscribe({})
  await transport.connect()
  let release
  port.writeGate = new Promise(resolve => { release = resolve })
  const writing = transport.writeBytes(new Uint8Array([0xff, 0, 0x1e]))
  const changing = transport.setBaudRate(2000000)
  await new Promise(resolve => setTimeout(resolve, 1))
  assert.equal(port.closes, 0)
  release()
  await writing
  await changing
  assert.deepEqual(port.writes[0], new Uint8Array([0xff, 0, 0x1e]))
  port.rejected.push(1500000)
  await assert.rejects(transport.setBaudRate(1500000), /unsupported speed/)
  await transport.setBaudRate(115200)
  assert.equal(transport.getDiagnostics().baudRate, 115200)
  assert.deepEqual(port.opens, [115200, 2000000, 1500000, 115200])
  await transport.disconnect()
})

test('physical browser disconnect is reported while internal reopen remains subscribed', async () => {
  const port = new Port(), transport = serialTransport({ port }), events = []
  transport.subscribe({ close: () => events.push('close') })
  await transport.connect()
  await transport.setBaudRate(921600)
  assert.deepEqual(events, [])
  port.dispatchEvent(new Event('disconnect'))
  assert.deepEqual(events, ['close'])
  await transport.disconnect()
})

test('native USB ports without modem control lines can still connect', async () => {
  const port = new Port()
  port.setSignals = async () => { throw Object.assign(new Error('No control lines'), { name: 'NotSupportedError' }) }
  const transport = serialTransport({ port })
  await transport.connect()
  await transport.writeBytes(new Uint8Array([1]))
  assert.deepEqual(port.writes, [new Uint8Array([1])])
  await transport.disconnect()
})

test('browser cancellation releases a port that finishes opening late', async () => {
  const port = new Port(), original = port.open.bind(port), transport = serialTransport({ port })
  let finish
  const started = new Promise(resolve => {
    port.open = async options => { await new Promise(release => { finish = release; resolve() }); await original(options) }
  })
  const controller = new AbortController()
  const connecting = transport.connect({ signal: controller.signal })
  await started
  controller.abort()
  await assert.rejects(connecting, { name: 'AbortError' })
  await transport.disconnect()
  finish()
  await new Promise(resolve => setTimeout(resolve, 1))
  assert.equal(port.closes, 1)
  assert.equal(port.readable, null)
})

test('an aborted retune cannot complete later at a stale rate after baseline recovery', async () => {
  const port = new Port(), original = port.open.bind(port), transport = serialTransport({ port }), controller = new AbortController()
  await transport.connect()
  let finish
  const started = new Promise(resolve => {
    port.open = async options => {
      if (options.baudRate === 2000000) await new Promise(release => { finish = release; resolve() })
      await original(options)
    }
  })
  const changing = transport.setBaudRate(2000000, { signal: controller.signal })
  await started
  controller.abort()
  const failed = assert.rejects(changing, { name: 'AbortError' })
  const recovery = transport.setBaudRate(115200)
  finish()
  await failed
  await recovery
  assert.deepEqual(port.opens, [115200, 2000000, 115200])
  assert.equal(port.closes, 2, 'the stale high-rate open is closed before baseline is acquired')
  assert.equal(transport.getDiagnostics().baudRate, 115200)
  await transport.disconnect()
})

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import {
  defaultPython, filterSerialPorts, inventory, listSerial, serialEntry, transportFor,
} from '../src/node.js'

test('the public Node entry point exposes passive inventory and serial adapters', () => {
  assert.equal(typeof defaultPython, 'string')
  assert.equal(typeof listSerial, 'function')
  assert.equal(typeof filterSerialPorts, 'function')
  assert.equal(typeof serialEntry, 'function')
  assert.equal(typeof transportFor, 'function')
  assert.equal(typeof inventory, 'function')
})

test('serial listing passes a bounded non-opening enumeration request to Python', async () => {
  const controller = new AbortController()
  let invocation
  const ports = await listSerial({ python: 'python-test', signal: controller.signal, timeout: 2345 }, {
    async execFile(command, args, options) {
      invocation = { command, args, options }
      return { stdout: '[{"path":"/dev/ttyACM0","vendorId":1234}]' }
    },
  })

  assert.deepEqual(ports, [{ path: '/dev/ttyACM0', vendorId: 1234 }])
  assert.equal(invocation.command, 'python-test')
  assert.match(invocation.args[0], /node_serial\.py$/)
  assert.deepEqual(invocation.args.slice(1), ['--list'])
  assert.equal(invocation.options.signal, controller.signal)
  assert.equal(invocation.options.timeout, 2345)
  assert.equal(invocation.options.maxBuffer, 1024 * 1024)
  assert.equal(invocation.options.windowsHide, true)
})

test('serial enumeration reports a bounded-process timeout and preserves cancellation', async () => {
  const timedOut = Object.assign(new Error('private process details'), { code: 'ETIMEDOUT' })
  await assert.rejects(
    listSerial({ timeout: 250 }, { execFile: async () => { throw timedOut } }),
    error => error.code === 'TIMEOUT' && error.cause === timedOut && !error.message.includes('private'),
  )

  const controller = new AbortController()
  const reason = new Error('cancelled')
  const pending = listSerial({ signal: controller.signal }, {
    execFile: async (_command, _args, options) => new Promise((_resolve, reject) => {
      options.signal.addEventListener('abort', () => reject(reason), { once: true })
    }),
  })
  controller.abort(reason)
  await assert.rejects(pending, error => error === reason)
})

test('serial candidate filtering stays platform-aware and classification is descriptor-only', () => {
  const ports = [
    { path: '/dev/ttyACM0', vendorId: 0x303a, productId: 0x1001, product: 'USB JTAG/serial' },
    { path: '/dev/ttyUSB0' },
    { path: '/dev/ttyS0' },
  ]
  assert.deepEqual(filterSerialPorts(ports, { platform: 'linux' }), ports.slice(0, 2))
  assert.deepEqual(filterSerialPorts(ports, { platform: 'linux', allPorts: true }), ports)
  const entry = serialEntry(ports[0])
  assert.equal(entry.interface, 'usj')
  assert.equal(entry.status, 'unprobed')
  assert.equal(entry.identity, null)
  const transport = transportFor(entry, { python: 'python-test' })
  assert.equal(typeof transport.connect, 'function')
})

test('inventory forwards timeout and cancellation without probing by default', async () => {
  const controller = new AbortController()
  let serialRequest
  let opened = 0
  const result = await inventory({ interfaces: ['serial'], timeout: 750, signal: controller.signal }, {
    platform: 'linux',
    async listSerial(request) {
      serialRequest = request
      return [{ path: '/dev/ttyACM1', vendorId: 0x303a, productId: 0x1001 }]
    },
    createDevice() {
      opened += 1
      throw new Error('passive inventory opened a serial port')
    },
  })

  assert.equal(serialRequest.timeout, 750)
  assert.equal(serialRequest.signal, controller.signal)
  assert.equal(opened, 0)
  assert.equal(result.devices.length, 1)
  assert.equal(result.devices[0].address, '/dev/ttyACM1')
  assert.equal(result.devices[0].status, 'unprobed')
  assert.equal(result.devices[0].identity, null)
})

test('explicit inventory probing acquires and reads metadata without interrupting the REPL', async () => {
  const controller = new AbortController()
  let connection, inspection, disconnected = false
  const result = await inventory({
    interfaces: ['serial'], probe: true, probeTimeout: 1250, signal: controller.signal,
  }, {
    platform: 'linux',
    async listSerial() { return [{ path: '/dev/ttyUSB7', vendorId: 0x1234 }] },
    transportFor(row) { return { address: row.address } },
    createDevice() {
      return {
        async connect(options) {
          connection = options
          return { identity: { deviceId: 'e8f60ae75642', board: 'TEST' } }
        },
        async inspect(options) { inspection = options; return { runtime: { vmReady: false } } },
        async disconnect() { disconnected = true },
      }
    },
  })

  assert.equal(connection.role, 'unknown')
  assert.equal(connection.timeout, 1250)
  assert.equal(connection.signal, controller.signal)
  assert.equal(connection.probeRepl, false)
  assert.equal(inspection.signal, controller.signal)
  assert.equal(disconnected, true)
  assert.equal(result.devices[0].status, 'identified')
  assert.deepEqual(result.devices[0].inspection, { runtime: { vmReady: false } })
})

test('inventory propagates an aborted serial enumeration', async () => {
  const reason = new Error('cancelled')
  await assert.rejects(
    inventory({ interfaces: ['serial'], signal: AbortSignal.abort(reason) }, {
      async listSerial({ signal }) { throw signal.reason },
    }),
    error => error === reason,
  )
})

// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { inventory, serialEntry } from '../src/node_inventory.js'
import { parseCli, runCli, formatResult } from '../src/cli.js'
const identity = { deviceId: 'abcdef123456', board: 'BOARD', firmwareVersion: 'v1', hostname: 'esp-vision-board' }
const ports = [{ path: '/dev/ttyACM0', vendorId: 0x303a, productId: 0x1001, serialNumber: 'abc' }, { path: '/dev/ttyUSB0', vendorId: 0x10c4, productId: 0xea60 }, { path: '/dev/ttyACM1', vendorId: 0x303a, productId: 0x4003, product: 'ESP-VISION BOARD' }, { path: '/dev/ttyS0' }]
const advertisement = { ...identity, addresses: ['192.168.1.7'], port: 3232, evMuxVersion: 4 }
const deps = { platform: 'linux', async listSerial() { return ports }, async listNetwork() { return [advertisement] } }
test('one inspect command supports Windows, Linux, macOS paths and strict command options', () => {
  for (const serial of ['COM3', '/dev/ttyACM0', '/dev/cu.usbserial']) assert.equal(parseCli(['inspect', '--port', serial]).endpoint.address, serial)
  assert.equal(parseCli(['inspect', '--tcp', '[::1]:3232']).endpoint.host, '::1')
  for (const args of [['discover'], ['info'], ['inspect', '--tcp', 'tcp://user:pass@host'], ['inspect', '--port', 'a', '--tcp', 'b'], ['inspect', '--fields', 'network'], ['control', '--port', 'a', '--action', 'reset'], ['configure', '--port', 'a', '--ssid', 'x', '--forget-wifi'], ['firmware','update','--catalog','x','--board','BOARD','--current-version','v1']]) assert.throws(() => parseCli(args))
})
test('passive inventory never creates a session, keeps unidentified ports and numbers all results', async () => {
  const result = await inventory({}, { ...deps, createDevice() { throw new Error('Passive enumeration opened a device') } })
  assert.equal(result.devices.length, 4)
  assert.equal(result.devices[0].status, 'advertised')
  assert.deepEqual(result.devices.map(row => row.number), [1, 2, 3, 4])
  const serial = result.devices.filter(row => row.interface !== 'tcp')
  assert.ok(serial.every(row => row.identity === null && row.status === 'unprobed'))
  assert.deepEqual(new Set(serial.map(row => row.interface)), new Set(['usj', 'usb', 'uart']))
})
test('USB product names are descriptor hints, never board identity or proof of firmware', () => {
  const row = serialEntry(ports[2]); assert.equal(row.interface, 'usb'); assert.equal(row.identity, null)
  assert.equal(serialEntry({ path: 'COM7', vendorId: 0x9999, product: 'Unknown ROM' }).interface, 'serial')
})
test('interface filters avoid unrelated discovery providers', async () => {
  const serial = await inventory({ interfaces: ['uart'] }, { ...deps, listNetwork() { throw new Error('Unexpected LAN access') } })
  assert.equal(serial.devices.length, 1); assert.deepEqual(serial.warnings, [])
  const tcp = await inventory({ interfaces: ['tcp'] }, { ...deps, listSerial() { throw new Error('Unexpected serial access') } })
  assert.equal(tcp.devices.length, 1); assert.deepEqual(tcp.warnings, [])
})
test('active probe groups one board across USB/TCP and retains failed ports at the end', async () => {
  let disconnects = 0
  const result = await inventory({ probe: true }, { ...deps, transportFor: row => row,
    createDevice({ transport }) { return { async connect() { if (transport.address === '/dev/ttyACM0') throw Object.assign(new Error('busy'), { code: 'PORT_BUSY' }); return { identity } }, async inspect() { return { identity } }, async disconnect() { disconnects++ } } } })
  assert.equal(disconnects, 4)
  assert.ok(result.devices.slice(0, 3).every(row => row.group === 1 && row.status === 'identified'))
  assert.equal(result.devices.at(-1).address, '/dev/ttyACM0'); assert.equal(result.devices.at(-1).error.code, 'PORT_BUSY')
})
test('partial discovery failure preserves valid results and empty successful discovery is valid', async () => {
  const result = await inventory({}, { ...deps, async listNetwork() { throw new Error('missing mDNS') } })
  assert.equal(result.sourcesAvailable, 1); assert.equal(result.devices.length, 3); assert.equal(result.warnings[0].source, 'tcp')
  const empty = await runCli(parseCli(['inspect']), { inventory: async () => ({ devices: [], warnings: [{ source: 'tcp' }], sourcesAvailable: 1 }) })
  assert.deepEqual(empty.devices, [])
})
test('targeted inspection calls only Core facade with selected fields and always disconnects', async () => {
  const calls = []
  const result = await runCli(parseCli(['inspect','--port','COM3','--fields','identity,capabilities']), { transportFor: row => row, createDevice() { return { async connect(options) { calls.push(options.role) }, async inspect(request) { calls.push(request.fields); return { identity } }, async disconnect() { calls.push('disconnect') } } } })
  assert.deepEqual(result.identity, identity); assert.deepEqual(calls, ['unknown', ['identity','capabilities'], 'disconnect'])
})
test('selection resolves the same numbered enumeration snapshot, with discovered identity check', async () => {
  let expected
  await runCli(parseCli(['inspect','--select','1']), { inventory: async () => ({ devices: [{ number: 1, interface: 'tcp', advertisement }], warnings: [], sourcesAvailable: 1 }), transportFor: row => row, createDevice() { return { async connect(request) { expected = request.expectedDevice }, async inspect() { return {} }, async disconnect() {} } } })
  assert.equal(expected, advertisement)
})
test('configure keeps Wi-Fi password out of results and forwards it only to configure', async () => {
  let patch, closed = false
  const result = await runCli(parseCli(['configure','--port','COM3','--ssid','WIFI','--password-stdin']), { transportFor: row => row, readPassword: async () => 'secret', createDevice() { return { async connect() {}, async configure(request) { patch = request.network }, async disconnect() { closed = true } } } })
  assert.equal(patch.credentials.password, 'secret'); assert.deepEqual(result, { ok: true }); assert.equal(closed, true)
})
test('failed device operation still releases transport and session', async () => {
  let closed = false
  await assert.rejects(runCli(parseCli(['inspect','--tcp','localhost']), { transportFor: row => row, createDevice() { return { async connect() {}, async inspect() { throw new Error('failed') }, async disconnect() { closed = true } } } }), /failed/)
  assert.equal(closed, true)
})
test('firmware CLI checks catalog/version then updates through the existing facade', async () => {
  const calls = [], selected = { board: 'BOARD', version: 'v2' }
  const result = await runCli(parseCli(['firmware','update','--catalog','catalog.json','--board','BOARD','--current-version','v1','--version','v2','--port','COM3']), { readFile: async () => '{"schemaVersion":1,"releases":[]}', openLoader() {}, createDevice() { return { firmware: { async check(request) { calls.push(request.version); return { release: selected } }, async update(request) { assert.equal(request.release, selected); return { verified: true } } }, async disconnect() { calls.push('disconnect') } } } })
  assert.equal(result.verified, true); assert.deepEqual(calls, ['v2', 'disconnect'])
})
test('terminal output escapes control characters in device-provided data', () => {
  const text = formatResult({ devices: [{ number: 1, address: 'COM3\x1b[31m', identity: { board: 'BOARD\nBAD' } }] })
  assert.ok(!text.includes('\x1b')); assert.ok(text.includes('BOARD?BAD'))
})

test('cancelled active enumeration still releases the probed session', async () => {
  const controller = new AbortController(); let closed = false
  await assert.rejects(inventory({ probe: true, signal: controller.signal, interfaces: ['usj'] }, { ...deps, transportFor: row => row, createDevice() { return { async connect() { controller.abort(new Error('cancelled')); throw controller.signal.reason }, async disconnect() { closed = true } } } }), /cancelled/)
  assert.equal(closed, true)
})

test('control passes the requested action and reset mode to Core without alias commands', async () => {
  let request
  const result = await runCli(parseCli(['control', '--port', 'COM3', '--action', 'reset', '--mode', 'soft']), { transportFor: row => row, createDevice() { return { async connect() {}, async control(value) { request = value; return { ok: true } }, async disconnect() {} } } })
  assert.equal(result.ok, true); assert.equal(request.action, 'reset'); assert.equal(request.mode, 'soft')
})

test('normal firmware commands need neither a catalog nor a user loader module', async () => {
  const options = parseCli(['firmware', 'update', '--port', 'COM3', '--version', 'new', '--board', 'BOARD'])
  let loaderPort, request
  await runCli(options, { transportFor: row => row, openLoader: ({ port }) => { loaderPort = port; return {} }, createDevice() { return { firmware: { async check(value) { request = value; return { release: { version: 'new' } } }, async update(value) { await value.openLoader({}); return { verified: true } } }, async disconnect() {} } } })
  assert.equal(request.catalog, undefined); assert.equal(loaderPort, 'COM3')
  assert.throws(() => parseCli(['firmware', 'update', '--port', 'COM3', '--version', 'new', '--loader', 'user.mjs']))
  assert.throws(() => parseCli(['inspect', '--serial', 'COM3']))
})

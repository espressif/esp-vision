// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { EventEmitter } from 'node:events'
import { test } from 'node:test'
import { createNodeDiscovery } from '../src/network_discovery_node.js'
import { NetworkDiscovery, createBridgeDiscovery } from '../src/network_discovery.js'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { EvRepl } from '../src/evrepl.js'
import { DeviceSession } from '../src/device_session.js'
const hostname = 'esp-vision-esp32-s3-eye-a1b2c3-0-9-1'
const instance = hostname + '._esp-vision._tcp.local'
const device = { deviceId: 'aabbcca1b2c3', hostname, board: 'ESP32_S3_EYE', firmwareVersion: '0.9.1', evMuxVersion: 4, port: 3232, addresses: ['192.168.2.3'] }
const records = [
  { name: '_esp-vision._tcp.local', type: 'PTR', ttl: 60, data: instance },
  { name: instance, type: 'SRV', ttl: 60, data: { target: hostname + '.local', port: 3232 } },
  { name: instance, type: 'TXT', ttl: 60, data: Object.entries({ txtvers: '1', id: device.deviceId, hostname, board: device.board, fw: device.firmwareVersion, evmux: '4' }).map(([k, v]) => Buffer.from(`${k}=${v}`)) },
  { name: hostname + '.local', type: 'A', ttl: 60, data: '192.168.2.3' },
]
function provider(t, emit) {
  const mdns = new EventEmitter()
  mdns.query = () => { if (!mdns.sent) { mdns.sent = true; emit(mdns) } }
  mdns.destroy = () => { mdns.closed = true }
  t.after(() => assert.equal(mdns.closed, true))
  return createNodeDiscovery({ createMdns: () => mdns })
}

test('split PTR/SRV/TXT/A records resolve, duplicate packets deduplicate and v4 metadata stays intact', async t => {
  const discovery = provider(t, mdns => queueMicrotask(() => {
    mdns.emit('response', { answers: records.slice(0, 2) })
    mdns.emit('response', { additionals: records.slice(2) })
    mdns.emit('response', { answers: records })
  }))
  const results = await discovery.list({ timeout: 50 })
  assert.equal(results.length, 1)
  assert.deepEqual({ ...results[0], expiresAt: undefined, compatible: undefined }, { ...device, expiresAt: undefined, compatible: undefined })
})

test('goodbye removes a device, and an expired address cannot appear online', async t => {
  let now = 1000
  t.mock.method(Date, 'now', () => now)
  const discovery = provider(t, mdns => queueMicrotask(() => {
    mdns.emit('response', { answers: records })
    mdns.emit('response', { answers: [{ ...records[0], ttl: 0 }] })
    now += 1001
  }))
  assert.deepEqual(await discovery.list({ timeout: 50 }), [])
})

test('cache flush preserves addresses split across a burst of packets', async t => {
  let now = 1000
  t.mock.method(Date, 'now', () => now)
  const discovery = provider(t, mdns => queueMicrotask(() => {
    mdns.emit('response', { answers: records })
    now += 500
    mdns.emit('response', { answers: [{ ...records[3], flush: true, data: '192.168.2.4' }] })
  }))
  assert.deepEqual((await discovery.list({ timeout: 50 }))[0].addresses.sort(), ['192.168.2.3', '192.168.2.4'])
})

test('old addresses expire after the cache-flush grace period', async t => {
  let now = 1000
  t.mock.method(Date, 'now', () => now)
  const discovery = provider(t, mdns => queueMicrotask(() => {
    mdns.emit('response', { answers: records })
    now += 2000
    mdns.emit('response', { answers: [{ ...records[3], flush: true, data: '192.168.2.4' }] })
    now += 1001
  }))
  assert.deepEqual((await discovery.list({ timeout: 50 }))[0].addresses, ['192.168.2.4'])
})

test('re-announcement rescues a record during the goodbye grace period', async t => {
  let now = 1000
  t.mock.method(Date, 'now', () => now)
  const discovery = provider(t, mdns => queueMicrotask(() => {
    mdns.emit('response', { answers: records })
    mdns.emit('response', { answers: [{ ...records[0], ttl: 0 }] })
    now += 500
    mdns.emit('response', { answers: [records[0]] })
    now += 1001
  }))
  assert.equal((await discovery.list({ timeout: 50 })).length, 1)
})

test('expired cache entries cannot block discovery at the record limit', async t => {
  let now = 1000
  t.mock.method(Date, 'now', () => now)
  const discovery = provider(t, mdns => queueMicrotask(() => {
    for (let start = 0; start < 2048; start += 256) mdns.emit('response', { answers: Array.from({ length: 256 }, (_, i) => ({
      name: `unrelated-${start + i}.local`, type: 'A', ttl: 1, data: '192.168.2.10',
    })) })
    now += 2000
    mdns.emit('response', { answers: records })
  }))
  assert.equal((await discovery.list({ timeout: 50 })).length, 1)
})

test('cancellation during adapter creation still closes the socket', async () => {
  const controller = new AbortController()
  const mdns = new EventEmitter()
  let closed = false
  mdns.destroy = () => { closed = true }
  mdns.query = () => assert.fail('aborted discovery must not query')
  const discovery = createNodeDiscovery({ createMdns() {
    controller.abort(new Error('cancel during setup'))
    return mdns
  } })
  await assert.rejects(discovery.list({ signal: controller.signal }), /cancel during setup/)
  assert.equal(closed, true)
})

test('SRV targets inconsistent with the advertised hostname are ignored', async t => {
  const discovery = provider(t, mdns => queueMicrotask(() => mdns.emit('response', { answers: [
    records[0], { ...records[1], data: { ...records[1].data, target: 'other.local' } }, records[2],
    { ...records[3], name: 'other.local' },
  ] })))
  assert.deepEqual(await discovery.list({ timeout: 50 }), [])
})

test('cancellation releases mDNS resources and does not resolve a partial device', async t => {
  const controller = new AbortController()
  const discovery = provider(t, () => queueMicrotask(() => controller.abort(new Error('cancel discovery'))))
  await assert.rejects(discovery.list({ timeout: 3000, signal: controller.signal }), /cancel discovery/)
})

test('unrelated/malformed advertisements and public destinations are ignored', async t => {
  const discovery = provider(t, mdns => queueMicrotask(() => mdns.emit('response', { answers: [
    ...records.slice(0, 3), { ...records[3], data: '8.8.8.8' },
  ] })))
  assert.deepEqual(await discovery.list({ timeout: 50 }), [])
})

test('browser bridge URLs stay on the configured origin and reject arbitrary forwarding URLs', async () => {
  const bridge = 'ws://127.0.0.1:8766/device'
  const handle = '/device/' + 'a'.repeat(32)
  const valid = createBridgeDiscovery(bridge, { fetch: async () => ({ ok: true, json: async () => ({ devices: [{ ...device, connectionUrl: handle }] }) }) })
  assert.equal((await valid.list({ timeout: 50 }))[0].connectionUrl, 'ws://127.0.0.1:8766' + handle)
  const invalid = createBridgeDiscovery(bridge, { fetch: async () => ({ ok: true, json: async () => ({ devices: [{ ...device, connectionUrl: 'ws://attacker.example' + handle }] }) }) })
  await assert.rejects(invalid.list({ timeout: 50 }), /Invalid discovered connection URL/)
  const controller = new AbortController(); controller.abort(new Error('already cancelled'))
  await assert.rejects(new NetworkDiscovery({ list() { assert.fail('must not browse') } }).list({ signal: controller.signal }), /already cancelled/)
})

test('a changed IP leading to the wrong device fails before any acquire RPC', async () => {
  const session = new DeviceSession({ transport: { writeBytes() {} }, EvMuxParser, makeEvMuxFrame, EvRepl })
  const requests = []
  session.rpc = async (method, payload) => { requests.push({ method, payload }); return { ...device, deviceId: '000000000000', evMuxVersion: 4 } }
  await assert.rejects(session.negotiate({ role: 'tcp', expectedDevice: device }), error => error.code === 'DEVICE_IDENTITY_MISMATCH')
  assert.deepEqual(requests, [{ method: 'hello', payload: {} }])
  assert.equal(session.sessionId, null)
  session.close()
})

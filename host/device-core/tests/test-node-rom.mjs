// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { EventEmitter } from 'node:events'
import { PassThrough, Writable } from 'node:stream'
import { openNodeRom } from '../src/node_rom.js'

function fixture({ mac = 'e8f60ae75642', expected = mac, prepare, initializeError } = {}) {
  const calls = [], requests = []
  const child = new EventEmitter()
  child.exitCode = null
  child.stdout = new PassThrough()
  child.stdin = new Writable({
    write(data, encoding, done) {
      const request = JSON.parse(data.toString())
      requests.push(request)
      queueMicrotask(() => child.stdout.write(JSON.stringify(request.method === 'initialize' && initializeError
        ? { id: request.id, error: 'ROM_OPERATION_FAILED', method: 'initialize', detail: initializeError }
        : { id: request.id, result: request.method === 'initialize'
          ? { chipFamily: 0x3280, chipRevision: 302, chipMac: mac, connectionMode: 'no-reset' } : null }) + '\n'))
      done()
    },
    final(done) { child.exitCode = 0; queueMicrotask(() => child.emit('exit', 0)); done() },
  })
  child.kill = () => { child.exitCode = 1; child.emit('exit', 1) }
  const selected = { port: '/dev/rebound-rom', expectedMac: expected, transition: { requested: true } }
  const dependencies = {
    async preparePort(options) { calls.push(['prepare', options]); return prepare ? prepare(options) : selected },
    spawn(...args) { calls.push(['spawn', ...args]); return child },
  }
  return { calls, requests, child, selected, dependencies }
}

test('Node ROM process uses the qualified rebound port and verifies its actual MAC before writes', async () => {
  const f = fixture(), loader = openNodeRom({ port: '/dev/runtime', python: 'test-python' }, f.dependencies)
  try {
    await loader.initialize()
    assert.equal(f.calls[1][2][2], '/dev/rebound-rom')
    assert.equal(loader.port, '/dev/rebound-rom')
    assert.equal(loader.chipMac, 'e8f60ae75642')
    assert.equal(loader.connectionMode, 'no-reset')
    assert.deepEqual(loader.usbTransition, { requested: true })
    await loader.flashData(new Uint8Array([1, 2]), () => {}, 65536)
    assert.deepEqual(f.requests.map(r => r.method), ['initialize', 'write'])
  } finally { await loader.disconnect() }
})

test('a ROM MAC mismatch closes the process and cannot be followed by a write', async () => {
  const f = fixture({ expected: '001122334455' }), loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
  await assert.rejects(loader.initialize(), { code: 'ROM_IDENTITY_CHANGED' })
  await assert.rejects(loader.flashData(new Uint8Array([1]), () => {}, 65536), { code: 'ROM_CLOSED' })
  assert.deepEqual(f.requests.map(r => r.method), ['initialize'])
  assert.equal(f.child.exitCode, 0)
})

test('initialization is single use and never repeats a bootloader handoff', async () => {
  const f = fixture(), loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
  try {
    await loader.initialize()
    await assert.rejects(loader.initialize(), { code: 'ROM_ALREADY_INITIALIZED' })
    assert.equal(f.calls.filter(c => c[0] === 'prepare').length, 1)
    assert.equal(f.calls.filter(c => c[0] === 'spawn').length, 1)
  } finally { await loader.disconnect() }
})

test('flash operations before initialization fail without spawning or sending a process request', async () => {
  const f = fixture(), loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
  await assert.rejects(loader.flashData(new Uint8Array([1]), () => {}, 65536), { code: 'ROM_NOT_INITIALIZED' })
  assert.equal(f.calls.length, 0)
  assert.equal(f.requests.length, 0)
  await loader.disconnect()
})

test('disconnect cancels a pending port handoff before any ROM process can start', async () => {
  const f = fixture({ prepare: ({ signal }) => new Promise((resolve, reject) => {
    signal.addEventListener('abort', () => reject(signal.reason), { once: true })
  }) })
  const loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
  const initializing = loader.initialize()
  const rejected = assert.rejects(initializing, { code: 'ROM_CLOSED' })
  await loader.disconnect()
  await rejected
  assert.equal(f.calls.filter(c => c[0] === 'spawn').length, 0)
})

test('handoff failure retains its cause and cannot replay on the same loader', async () => {
  const reason = Object.assign(new Error('ROM did not re-enumerate'), { code: 'ROM_PORT_LOST' })
  const f = fixture({ prepare: () => { throw reason } }), loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
  await assert.rejects(loader.initialize(), error => error === reason)
  await assert.rejects(loader.initialize(), { code: 'ROM_CLOSED' })
  assert.equal(f.calls.length, 1)
})

test('underlying ROM initialization diagnostics survive cleanup and prevent writes', async () => {
  const f = fixture({ initializeError: 'No serial data received' }), loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
  await assert.rejects(loader.initialize(), error => error.code === 'ROM_OPERATION_FAILED'
    && error.operation === 'initialize' && error.cause === 'No serial data received')
  await assert.rejects(loader.flashData(new Uint8Array([1]), () => {}, 65536), { code: 'ROM_CLOSED' })
  assert.equal(f.requests.length, 1)
})

test('one read-only ROM sync failure may hand off the verified same runtime before any write', async () => {
  const first = fixture({ initializeError: 'No serial data received' }), second = fixture()
  const oldPort = { port: '/dev/rom', expectedMac: 'e8f60ae75642', transition: { observed: 'rom', requested: false } }
  const rebound = { port: '/dev/rebound', expectedMac: oldPort.expectedMac, transition: { observed: 'rom', requested: true } }
  let starts = 0, recoveries = 0
  const loader = openNodeRom({ port: '/dev/rom' }, {
    preparePort: async () => oldPort,
    recoverPort: async previous => { assert.equal(previous, oldPort); recoveries++; return rebound },
    spawn: () => (++starts === 1 ? first.child : second.child),
  })
  try {
    await loader.initialize()
    assert.equal(starts, 2)
    assert.equal(recoveries, 1)
    assert.equal(loader.usbTransition.previousInitialization.detail, 'No serial data received')
    assert.deepEqual(first.requests.map(r => r.method), ['initialize'])
    assert.deepEqual(second.requests.map(r => r.method), ['initialize'])
    await loader.flashData(new Uint8Array([1]), () => {}, 65536)
    assert.deepEqual(second.requests.map(r => r.method), ['initialize', 'write'])
  } finally { await loader.disconnect() }
})

test('an initialization that already requested bootloader cannot request it again', async () => {
  const f = fixture({ initializeError: 'Still no serial data' })
  f.selected.transition = { observed: 'rom', requested: true }
  let recoveries = 0
  const loader = openNodeRom({ port: '/dev/runtime' }, { ...f.dependencies,
    recoverPort: async () => { recoveries++; throw Error('Must not replay bootloader') },
  })
  await assert.rejects(loader.initialize(), error => error.cause === 'Still no serial data')
  assert.equal(recoveries, 0)
  assert.equal(f.calls.filter(c => c[0] === 'spawn').length, 1)
})

test('absence of a confirmed runtime preserves the initial ROM sync failure without replay', async () => {
  const f = fixture({ initializeError: 'No serial data received' })
  f.selected.transition = { observed: 'rom', requested: false }
  const loader = openNodeRom({ port: '/dev/rom' }, { ...f.dependencies, recoverPort: async () => null })
  await assert.rejects(loader.initialize(), error => error.code === 'ROM_OPERATION_FAILED'
    && error.operation === 'initialize' && error.cause === 'No serial data received')
  assert.equal(f.requests.length, 1)
})

test('a second initialization sync failure cannot trigger a third process or handoff', async () => {
  const first = fixture({ initializeError: 'First sync failed' }), second = fixture({ initializeError: 'Second sync failed' })
  const selected = { port: '/dev/rom', expectedMac: 'e8f60ae75642', transition: { observed: 'rom', requested: false } }
  let starts = 0, recoveries = 0
  const loader = openNodeRom({ port: '/dev/rom' }, {
    preparePort: async () => selected,
    recoverPort: async () => { recoveries++; return { ...selected, transition: { observed: 'rom', requested: true } } },
    spawn: () => (++starts === 1 ? first.child : second.child),
  })
  await assert.rejects(loader.initialize(), error => error.cause === 'Second sync failed'
    && error.transition.previousInitialization.detail === 'First sync failed')
  assert.equal(starts, 2)
  assert.equal(recoveries, 1)
  assert.equal(first.requests.length + second.requests.length, 2)
})

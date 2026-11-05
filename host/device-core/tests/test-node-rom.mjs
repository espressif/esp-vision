// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { EventEmitter } from 'node:events'
import { PassThrough, Writable } from 'node:stream'
import { openNodeRom } from '../src/node_rom.js'

function fixture({ mac = 'e8f60ae75642', expected = mac, prepare, initializeError, operationError, readResult = 'QUI=', stubResult = { stubRunning: true }, holdErase = false } = {}) {
  const calls = [], requests = []
  const child = new EventEmitter()
  child.exitCode = null
  child.stdout = new PassThrough()
  child.stdin = new Writable({
    write(data, encoding, done) {
      const request = JSON.parse(data.toString())
      requests.push(request)
      const detail = request.method === 'initialize' ? initializeError
        : typeof operationError === 'function' ? operationError(request) : operationError
      if (request.method === 'eraseRegion' && holdErase) { done(); return }
      queueMicrotask(() => child.stdout.write(JSON.stringify(detail
        ? { id: request.id, error: 'ROM_OPERATION_FAILED', method: request.method, detail }
        : { id: request.id, result: request.method === 'initialize'
          ? { chipFamily: 0x3280, chipRevision: 302, chipMac: mac, connectionMode: 'no-reset' }
          : request.method === 'read' ? readResult : request.method === 'stub' ? stubResult : null }) + '\n'))
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


test('post-initialize read or write failures never start recovery, a second process, or replay data', async () => {
  for (const method of ['read', 'write']) {
    const f = fixture({ operationError: 'Original operation failed' })
    f.selected.transition = { observed: 'rom', requested: false }
    let recoveries = 0, progresses = 0
    const loader = openNodeRom({ port: '/dev/rom' }, { ...f.dependencies,
      recoverPort: async () => { recoveries++; throw Error('Read/write must not recover') },
    })
    try {
      await loader.initialize()
      const operation = method === 'read' ? loader.readFlash(0x8000, 1024)
        : loader.flashData(new Uint8Array([1, 2, 3]), () => { progresses++ }, 0x10000)
      await assert.rejects(operation, error => error.code === 'ROM_OPERATION_FAILED'
        && error.operation === method && error.cause === 'Original operation failed')
      assert.equal(recoveries, 0)
      assert.equal(progresses, 0)
      assert.equal(f.calls.filter(c => c[0] === 'spawn').length, 1)
      assert.deepEqual(f.requests.map(r => r.method), ['initialize', method])
    } finally { await loader.disconnect() }
  }
})


test('Node ROM reads forward default and explicit budgets and report progress only after complete data', async () => {
  const f = fixture(), loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
  const progress = []
  try {
    await loader.initialize()
    assert.deepEqual(await loader.readFlash(0x8000, 2, (...args) => progress.push(args)), new Uint8Array([65, 66]))
    assert.deepEqual(await loader.readFlash(0x10000, 2, undefined,
      { chunkSize: 65536, blockSize: 4096, maxInFlight: 2 }), new Uint8Array([65, 66]))
    assert.deepEqual(f.requests.map(r => [r.method, r.args]), [['initialize', []],
      ['read', [0x8000, 2, { blockSize: 1024, maxInFlight: 1 }]],
      ['read', [0x10000, 2, { blockSize: 4096, maxInFlight: 2 }]]])
    assert.equal(progress.length, 1)
    assert.deepEqual(progress[0], [new Uint8Array([65, 66]), 2, 2])
  } finally { await loader.disconnect() }
})

test('invalid read ranges, progress, or packet budgets never issue a child process RPC', async () => {
  const f = fixture(), loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
  try {
    await loader.initialize()
    for (const [offset, count, progress, options] of [[-1, 2], [0, 0], [0, 0x100000000], [0xffffffff, 2], [0, 2, null],
      [0, 2, undefined, null], [0, 2, undefined, []], [0, 2, undefined, { blockSize: 512 }],
      [0, 2, undefined, { blockSize: true }], [0, 2, undefined, { blockSize: null }], [0, 2, undefined, { maxInFlight: 0 }],
      [0, 2, undefined, { maxInFlight: 65 }], [0, 2, undefined, { maxInFlight: null }], [0, 2, undefined, { maxInFlight: 1.5 }],
      [0, 2, undefined, { unknown: 1 }], [0, 2, undefined, { chunkSize: 1 }]]) {
      await assert.rejects(loader.readFlash(offset, count, progress, options), error => error instanceof TypeError || error instanceof RangeError)
    }
    assert.deepEqual(f.requests.map(r => r.method), ['initialize'])
  } finally { await loader.disconnect() }
})

test('incomplete read results are rejected without progress, recovery, or duplicate read', async () => {
  const f = fixture({ readResult: 'QQ==' })
  let recovered = 0, progressed = 0
  const loader = openNodeRom({ port: '/dev/runtime' }, { ...f.dependencies,
    recoverPort: async () => { recovered++; throw Error('Read must not recover') },
  })
  try {
    await loader.initialize()
    await assert.rejects(loader.readFlash(0, 2, () => { progressed++ }), { code: 'ROM_READ_LENGTH', operation: 'read' })
    assert.equal(recovered, 0)
    assert.equal(progressed, 0)
    assert.deepEqual(f.requests.map(r => r.method), ['initialize', 'read'])
  } finally { await loader.disconnect() }
})

test('Node region erase requires the validated stub and sends only the exact qualified range', async () => {
  const f = fixture(), loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
  try {
    await assert.rejects(loader.eraseRegion(0x710000, 4096), { code: 'ROM_NOT_INITIALIZED' })
    await loader.initialize()
    await assert.rejects(loader.eraseRegion(0x710000, 4096), { code: 'ROM_STUB_REQUIRED' })
    const stub = await loader.runStub()
    assert.equal(stub, loader); assert.equal(stub.IS_STUB, true)
    await stub.eraseRegion(0x710000, 0x8f0000)
    await stub.eraseRegion(0xfffff000, 4096)
    assert.deepEqual(f.requests.map(r => [r.method, r.args]), [
      ['initialize', []], ['stub', []], ['eraseRegion', [0x710000, 0x8f0000]],
      ['eraseRegion', [0xfffff000, 4096]],
    ])
  } finally { await loader.disconnect() }
  assert.equal(loader.IS_STUB, false)
  await assert.rejects(loader.eraseRegion(0x710000, 4096), { code: 'ROM_CLOSED' })
})

test('invalid Node erase ranges issue no child request', async () => {
  const f = fixture(), loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
  try {
    await loader.initialize(); await loader.runStub()
    for (const [address, size] of [[-4096, 4096], [1, 4096], [0, 0], [0, 1], [0, 0x100000000],
      [0xfffff000, 8192], [NaN, 4096], [true, 4096], [0, true], [0x100000000, 4096]]) {
      await assert.rejects(loader.eraseRegion(address, size), RangeError)
    }
    assert.deepEqual(f.requests.map(r => r.method), ['initialize', 'stub'])
  } finally { await loader.disconnect() }
})

test('unconfirmed Node stub responses close the process before any erase', async () => {
  for (const stubResult of [null, {}, { stubRunning: false }, { stubRunning: 1 }]) {
    const f = fixture({ stubResult }), loader = openNodeRom({ port: '/dev/runtime' }, f.dependencies)
    await loader.initialize()
    await assert.rejects(loader.runStub(), { code: 'ROM_STUB_REQUIRED' })
    await assert.rejects(loader.eraseRegion(0x710000, 4096), { code: 'ROM_CLOSED' })
    assert.deepEqual(f.requests.map(r => r.method), ['initialize', 'stub'])
    assert.equal(f.child.exitCode, 0)
  }
})

test('Node erase failure keeps official diagnostics and prevents later writes or recovery', async () => {
  const f = fixture({ operationError: request => request.method === 'eraseRegion' ? 'Erase command rejected' : undefined })
  let recoveries = 0
  const loader = openNodeRom({ port: '/dev/runtime' }, { ...f.dependencies,
    recoverPort: async () => { recoveries++; throw new Error('Erase must not recover') },
  })
  await loader.initialize(); await loader.runStub()
  await assert.rejects(loader.eraseRegion(0x710000, 4096), error => error.code === 'ROM_OPERATION_FAILED'
    && error.operation === 'eraseRegion' && error.cause === 'Erase command rejected')
  await assert.rejects(loader.flashData(new Uint8Array([1]), () => {}, 0), { code: 'ROM_CLOSED' })
  await assert.rejects(loader.eraseRegion(0x710000, 4096), { code: 'ROM_CLOSED' })
  assert.equal(recoveries, 0)
  assert.deepEqual(f.requests.map(r => r.method), ['initialize', 'stub', 'eraseRegion'])
})

test('Node erase cancellation or port loss closes the in-flight request without success or retry', async () => {
  for (const cancelled of [true, false]) {
    const f = fixture({ holdErase: true }), controller = new AbortController()
    const loader = openNodeRom({ port: '/dev/runtime', signal: controller.signal }, f.dependencies)
    await loader.initialize(); await loader.runStub()
    const erasing = loader.eraseRegion(0x710000, 4096)
    const failed = assert.rejects(erasing, { code: cancelled ? 'CANCELLED' : 'ROM_CLOSED' })
    if (cancelled) controller.abort()
    else { f.child.exitCode = 1; f.child.emit('exit', 1) }
    await failed
    await assert.rejects(loader.flashData(new Uint8Array([1]), () => {}, 0), { code: 'ROM_CLOSED' })
    assert.deepEqual(f.requests.map(r => r.method), ['initialize', 'stub', 'eraseRegion'])
  }
})

test('a previously cancelled Node erase never sends a command', async () => {
  const f = fixture(), controller = new AbortController()
  const loader = openNodeRom({ port: '/dev/runtime', signal: controller.signal }, f.dependencies)
  await loader.initialize(); await loader.runStub(); controller.abort()
  await assert.rejects(loader.eraseRegion(0x710000, 4096), { name: 'AbortError' })
  assert.deepEqual(f.requests.map(r => r.method), ['initialize', 'stub'])
  assert.equal(f.child.exitCode, 0)
})

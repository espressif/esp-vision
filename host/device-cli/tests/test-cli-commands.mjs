// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { resolve } from 'node:path'
import { cliCapabilities, failureEnvelope, parseCli, runCli, successEnvelope } from '../src/cli.js'

function harness(overrides = {}) {
  const calls = [], events = []
  const listeners = new Map()
  const device = {
    async connect(request) { calls.push(['connect', request]); await overrides.onConnect?.(device) },
    async disconnect() { calls.push(['disconnect']); await overrides.disconnect?.() },
    on(event, handler) {
      const handlers = listeners.get(event) ?? new Set()
      handlers.add(handler)
      listeners.set(event, handlers)
      return () => handlers.delete(handler)
    },
    emit(event, value) { for (const handler of listeners.get(event) ?? []) handler(value) },
    listenerCount(event) { return listeners.get(event)?.size ?? 0 },
    async inspect(request) { calls.push(['inspect', request]); return { identity: { board: 'TEST' } } },
    async configure(request) { calls.push(['configure', request]); return { ok: true } },
    async control(request) { calls.push(['control', request]); return { ok: true } },
    firmware: {
      async check(request) { calls.push(['firmware.check', request]); return { release: { version: 'new' } } },
      async update(request) { calls.push(['firmware.update', request]); return { verified: true } },
    },
    python: {
      async execute(request) {
        calls.push(['python.execute', request])
        if (overrides.python) return overrides.python(request)
        return { ok: true, path: request.path ?? null, output: 'hello\n', outputTruncated: false }
      },
    },
    files: {
      async list(request) { calls.push(['files.list', request]); return [{ path: '/main.py', type: 'file', size: 5 }] },
      async read(request) { calls.push(['files.read', request]); return new Uint8Array([0, 255, 65]) },
      async write(request) { calls.push(['files.write', request]); return { ok: true, bytes: request.data.byteLength } },
      async remove(request) { calls.push(['files.remove', request]); return { ok: true } },
      async mkdir(request) { calls.push(['files.mkdir', request]); return { ok: true } },
    },
    async capture(request) {
      calls.push(['capture', request])
      if (overrides.capture) return overrides.capture(request)
      return { data: new Uint8Array([255, 216, 0, 217]), mimeType: 'image/jpeg', metadata: { width: 2, height: 1 } }
    },
  }
  return {
    calls,
    events,
    device,
    dependencies: {
      createDevice() { return device },
      transportFor: endpoint => endpoint,
      onEvent(event) { events.push(event) },
      ...overrides.dependencies,
    },
  }
}

test('connect negotiation is passive for RPC-only paths and active for REPL-dependent commands', async () => {
  const assertProbePolicy = async (args, expected, extraDependencies = {}) => {
    const h = harness()
    await runCli(parseCli(args), {
      ...h.dependencies,
      async readFile(path, encoding) {
        if (extraDependencies.readFile) return extraDependencies.readFile(path, encoding)
        return new Uint8Array([7])
      },
      async writeFile() {},
      ...extraDependencies,
    })
    const connect = h.calls.find(([name]) => name === 'connect')
    assert.ok(connect, `${args.join(' ')} did not connect`)
    assert.equal(connect[1].probeRepl, expected, args.join(' '))
  }

  for (const args of [
    ['inspect', '--port', 'COM3'],
    ['configure', '--port', 'COM3', '--network-enabled', 'false'],
    ['firmware', 'check', '--port', 'COM3'],
    ['files', 'write', '/upload.bin', '--file', 'upload.bin', '--port', 'COM3'],
    ['capture', 'camera', '--output', 'frame.jpg', '--port', 'COM3'],
    ['watch', '--duration', '1', '--port', 'COM3'],
  ]) await assertProbePolicy(args, false)

  for (const args of [
    ['python', '--source', 'pass', '--port', 'COM3'],
    ['files', 'list', '--port', 'COM3'],
    ['files', 'read', '/source.py', '--output', 'source.py', '--port', 'COM3'],
    ['files', 'remove', '/source.py', '--port', 'COM3'],
    ['files', 'mkdir', '/data', '--port', 'COM3'],
    ['control', '--action', 'stop', '--port', 'COM3'],
    ['firmware', 'update', '--port', 'COM3', '--version', 'v1'],
  ]) await assertProbePolicy(args, true)

  const planFile = resolve('plans/python.json')
  await assertProbePolicy(['batch', planFile, '--port', 'COM3'], true, {
    async readFile(path, encoding) {
      assert.equal(path, planFile)
      assert.equal(encoding, 'utf8')
      return JSON.stringify({ schemaVersion: 1, operations: [{ op: 'python', source: 'pass' }] })
    },
  })
})

test('Python input is exclusive, timeout is positive, and detached completion requires a device path', () => {
  assert.equal(parseCli(['python', '--source', 'print(42)', '--port', 'COM3']).source, 'print(42)')
  assert.equal(parseCli(['python', '--file', 'local.py', '--port', 'COM3', '--timeout', '1']).timeout, 1)
  assert.equal(parseCli(['python', '--path', '/main.py', '--completion', 'start', '--port', 'COM3']).completion, 'start')
  for (const args of [
    ['python', '--port', 'COM3'],
    ['python', '--source', 'x', '--file', 'y', '--port', 'COM3'],
    ['python', '--source', 'x', '--timeout', '0', '--port', 'COM3'],
    ['python', '--source', 'x', '--completion', 'start', '--port', 'COM3'],
    ['python', '--path', '/main.py', '--completion', 'start', '--timeout', '50', '--port', 'COM3'],
  ]) assert.throws(() => parseCli(args))
})

test('Python local file is read as UTF-8 and returned output is reported on the event stream', async () => {
  const h = harness(), localFile = resolve('hello.py')
  const options = parseCli(['python', '--file', localFile, '--port', 'COM3', '--timeout', '5'])
  const result = await runCli(options, {
    ...h.dependencies,
    async readFile(path, encoding) { assert.equal(path, localFile); assert.equal(encoding, 'utf8'); return 'print("from file")' },
  })
  assert.equal(h.calls.find(([name]) => name === 'python.execute')[1].source, 'print("from file")')
  assert.equal(h.calls.find(([name]) => name === 'python.execute')[1].timeout, 5)
  assert.equal(result.outputChars, 6)
  assert.ok(!Object.hasOwn(result, 'output'))
  assert.ok(h.events.some(event => event.event === 'python.output' && event.data === 'hello\n'))
  assert.ok(h.events.some(event => event.event === 'duration'))
  assert.deepEqual(h.calls.map(([name]) => name), ['connect', 'python.execute', 'disconnect'])
})

test('Python output, logs, and state stream before completion with a bounded output and one truncation marker', async () => {
  let resolveExecution, markStarted
  const started = new Promise(resolve => { markStarted = resolve })
  const h = harness({ python: async request => {
    assert.equal(request.maxOutputChars, 16384)
    markStarted()
    return new Promise(resolve => { resolveExecution = resolve })
  } })
  let finished = false
  const running = runCli(parseCli(['python', '--source', 'long_task()', '--port', 'COM3']), h.dependencies)
  running.then(() => { finished = true }, () => { finished = true })
  await started

  h.device.emit('python.output', 'x'.repeat(16394))
  h.device.emit('log', 'device log')
  h.device.emit('state', { state: 'running' })

  const outputEvents = h.events.filter(event => event.event === 'python.output')
  assert.equal(finished, false)
  assert.equal(outputEvents.length, 2)
  assert.equal(outputEvents[0].data.length, 16384)
  assert.equal(outputEvents[0].truncated, false)
  assert.deepEqual(outputEvents[1], { schemaVersion: 1, event: 'python.output', data: '', truncated: true, maxOutputChars: 16384 })
  assert.ok(h.events.some(event => event.event === 'python.log' && event.data === 'device log'))
  assert.ok(h.events.some(event => event.event === 'python.state' && event.data.state === 'running'))

  resolveExecution({ ok: true, output: 'x'.repeat(16384), outputTruncated: true })
  const result = await running
  assert.equal(result.outputChars, 16384)
  assert.equal(h.events.filter(event => event.event === 'python.output').length, 2)
  assert.equal(h.device.listenerCount('python.output'), 0)
  assert.equal(h.device.listenerCount('log'), 0)
  assert.equal(h.device.listenerCount('state'), 0)
})

test('Python event listeners are removed when execution fails', async () => {
  const failure = Object.assign(new Error('Python execution failed'), { code: 'PYTHON_FAILED' })
  let h
  h = harness({ python: async () => {
    h.device.emit('python.output', 'before failure')
    h.device.emit('log', 'failure context')
    throw failure
  } })
  await assert.rejects(runCli(parseCli(['python', '--source', 'fail()', '--port', 'COM3']), h.dependencies), error => error === failure)
  assert.equal(h.device.listenerCount('python.output'), 0)
  assert.equal(h.device.listenerCount('log'), 0)
  assert.equal(h.device.listenerCount('state'), 0)
})

test('completion start acknowledges a path and consumes disconnect-induced completion rejection', async () => {
  let rejectDone
  const done = new Promise((_resolve, reject) => { rejectDone = reject })
  const disconnectFailure = Object.assign(new Error('EV-MUX session released'), { code: 'CONNECTION_CLOSED' })
  const h = harness({
    python: async request => ({ ok: true, path: request.path, done }),
    disconnect() { rejectDone(disconnectFailure) },
  })
  const result = await runCli(parseCli(['python', '--path', '/run.py', '--completion', 'start', '--port', 'COM3']), h.dependencies)
  assert.deepEqual(result, { ok: true, started: true, path: '/run.py', durationMs: result.durationMs })
  assert.equal(h.calls.find(([name]) => name === 'connect')[1].probeRepl, true)
  assert.ok(h.calls.some(([name, request]) => name === 'python.execute' && request.path === '/run.py' && request.completion === 'start'))
  assert.equal(h.calls.at(-1)[0], 'disconnect')
  await new Promise(resolve => setImmediate(resolve))
  assert.ok(!h.events.some(event => ['python.completed', 'python.failed'].includes(event.event)))
})

test('completion start does not report a fast done fulfillment or rejection as a job outcome', async () => {
  const cases = [
    ['fulfilled', () => Promise.resolve({ ok: true, path: '/run.py' })],
    ['rejected', () => Promise.reject(Object.assign(new Error('connection closed'), { code: 'CONNECTION_CLOSED' }))],
  ]
  for (const [label, makeDone] of cases) {
    const done = makeDone()
    const h = harness({ python: async request => ({ ok: true, path: request.path, done }) })
    const result = await runCli(parseCli(['python', '--path', '/run.py', '--completion', 'start', '--port', 'COM3']), h.dependencies)
    assert.equal(result.ok, true, `${label}: start acknowledgement should succeed`)
    assert.equal(result.started, true, `${label}: start acknowledgement should be marked started`)
    assert.equal(h.calls.at(-1)[0], 'disconnect', `${label}: connection was not released`)
    await new Promise(resolve => setImmediate(resolve))
    assert.ok(!h.events.some(event => ['python.completed', 'python.failed'].includes(event.event)), `${label}: later completion was reported`)
  }
})

test('completion start still fails when the start operation itself is rejected and disconnects', async () => {
  const startupFailure = Object.assign(new Error('script start RPC failed'), { code: 'START_FAILED' })
  const h = harness({ python: async () => { throw startupFailure } })
  await assert.rejects(
    runCli(parseCli(['python', '--path', '/run.py', '--completion', 'start', '--port', 'COM3']), h.dependencies),
    failure => failure === startupFailure,
  )
  assert.equal(h.calls.at(-1)[0], 'disconnect')
  assert.ok(!h.events.some(event => ['python.completed', 'python.failed'].includes(event.event)))
})

test('completion start still reports a real disconnect cleanup failure', async () => {
  let rejectDone
  const done = new Promise((_resolve, reject) => { rejectDone = reject })
  const cleanupFailure = Object.assign(new Error('release failed'), { code: 'RELEASE_FAILED' })
  const h = harness({
    python: async request => ({ ok: true, path: request.path, done }),
    disconnect() {
      rejectDone(Object.assign(new Error('session closed'), { code: 'CONNECTION_CLOSED' }))
      throw cleanupFailure
    },
  })
  await assert.rejects(
    runCli(parseCli(['python', '--path', '/run.py', '--completion', 'start', '--port', 'COM3']), h.dependencies),
    failure => failure === cleanupFailure,
  )
  assert.equal(h.calls.at(-1)[0], 'disconnect')
  await new Promise(resolve => setImmediate(resolve))
  assert.ok(!h.events.some(event => ['python.completed', 'python.failed'].includes(event.event)))
})

test('device files move binary bytes through local files and return only bounded metadata', async () => {
  const h = harness(), written = []
  const readOptions = parseCli(['files', 'read', '/blob.bin', '--output', 'download.bin', '--max-bytes', '4', '--port', 'COM3'])
  const read = await runCli(readOptions, { ...h.dependencies, async writeFile(path, bytes) { written.push([path, [...bytes]]) } })
  assert.deepEqual(written, [['download.bin', [0, 255, 65]]])
  assert.deepEqual(read, { path: '/blob.bin', output: 'download.bin', bytes: 3 })

  const local = resolve('upload.bin')
  const writeOptions = parseCli(['files', 'write', '/upload.bin', '--file', local, '--port', 'COM3'])
  await runCli(writeOptions, { ...h.dependencies, async readFile(path) { assert.equal(path, local); return Buffer.from([9, 0, 255]) } })
  const write = h.calls.find(([name]) => name === 'files.write')[1]
  assert.deepEqual([...write.data], [9, 0, 255])
  assert.equal(write.path, '/upload.bin')

  for (const args of [
    ['files', 'read', '/a', '--port', 'COM3'],
    ['files', 'write', '/a', '--port', 'COM3'],
    ['files', 'remove', '/a', '--type', 'unknown', '--port', 'COM3'],
  ]) assert.throws(() => parseCli(args))
})

test('capture writes JPEG bytes to a local file and returns image metadata', async () => {
  const h = harness(), writes = []
  const result = await runCli(parseCli(['capture', 'camera', '--quality', '80', '--output', 'frame.jpg', '--port', 'COM3']), {
    ...h.dependencies,
    async writeFile(path, data) { writes.push([path, [...data]]) },
  })
  assert.deepEqual(writes, [['frame.jpg', [255, 216, 0, 217]]])
  assert.deepEqual(result, { source: 'camera', mimeType: 'image/jpeg', metadata: { width: 2, height: 1 }, output: 'frame.jpg', bytes: 4 })
  assert.equal(h.calls.find(([name]) => name === 'capture')[1].quality, 80)
})

test('busy failures are preserved and the device is released', async () => {
  const busy = Object.assign(new Error('Device Python interpreter is busy'), { code: 'DEVICE_BUSY' })
  const h = harness({ python: async () => { throw busy } })
  await assert.rejects(runCli(parseCli(['python', '--source', 'pass', '--port', 'COM3']), h.dependencies), error => error === busy)
  assert.equal(h.calls.at(-1)[0], 'disconnect')
})

test('signal cancellation is forwarded to Python and the connection is still released', async () => {
  const controller = new AbortController()
  const h = harness({ python: async request => {
    assert.equal(request.signal, controller.signal)
    const cancelled = Object.assign(new Error('cancelled'), { code: 'CANCELLED' })
    controller.abort(cancelled)
    throw cancelled
  } })
  await assert.rejects(runCli(parseCli(['python', '--source', 'while True: pass', '--port', 'COM3']), {
    ...h.dependencies, signal: controller.signal,
  }), { code: 'CANCELLED' })
  assert.equal(h.calls.at(-1)[0], 'disconnect')
  assert.equal(h.device.listenerCount('python.output'), 0)
  assert.equal(h.device.listenerCount('log'), 0)
  assert.equal(h.device.listenerCount('state'), 0)
})

test('cleanup failure is reported separately without replacing the operation failure', async () => {
  const operationFailure = Object.assign(new Error('device operation failed'), { code: 'DEVICE_BUSY' })
  const h = harness({ python: async () => { throw operationFailure }, async disconnect() { throw Object.assign(new Error('release failed'), { code: 'RELEASE_FAILED' }) } })
  await assert.rejects(runCli(parseCli(['python', '--source', 'pass', '--port', 'COM3']), h.dependencies), failure => {
    assert.equal(failure, operationFailure)
    assert.deepEqual(failure.cleanupError, { code: 'RELEASE_FAILED', message: 'release failed' })
    const envelope = failureEnvelope('python', failure)
    assert.equal(envelope.error.code, 'DEVICE_BUSY')
    assert.equal(envelope.cleanupError.code, 'RELEASE_FAILED')
    return true
  })
})

test('batch validates the plan, runs once in order, stops at first failure, and releases ownership', async () => {
  const failure = Object.assign(new Error('camera is busy'), { code: 'DEVICE_BUSY' })
  const h = harness({ capture: async () => { throw failure } })
  const planFile = resolve('plans/task.json')
  const plan = {
    schemaVersion: 1,
    operations: [
      { op: 'python', source: 'print(1)' },
      { op: 'files.mkdir', path: '/data', recursive: true },
      { op: 'capture', source: 'camera', output: './frame.jpg' },
      { op: 'files.remove', path: '/should-not-run' },
    ],
  }
  await assert.rejects(runCli(parseCli(['batch', planFile, '--port', 'COM3']), {
    ...h.dependencies,
    async readFile(path, encoding) { assert.equal(path, planFile); assert.equal(encoding, 'utf8'); return JSON.stringify(plan) },
  }), error => {
    assert.equal(error.code, 'DEVICE_BUSY')
    assert.deepEqual(error.batchReport, {
      schemaVersion: 1, ok: false, command: 'batch',
      completed: [
        { index: 1, operation: 'python', result: { ok: true, path: null, outputTruncated: false, outputChars: 6, durationMs: error.batchReport.completed[0].result.durationMs } },
        { index: 2, operation: 'files.mkdir', result: { ok: true } },
      ],
      failed: { index: 3, operation: 'capture', error: { code: 'DEVICE_BUSY', message: 'camera is busy' } },
    })
    return true
  })
  assert.deepEqual(h.calls.map(([name]) => name), ['connect', 'python.execute', 'files.mkdir', 'capture', 'disconnect'])
  assert.equal(h.calls.filter(([name]) => name === 'connect').length, 1)
  assert.ok(!h.calls.some(([name, request]) => name === 'files.remove' && request.path === '/should-not-run'))
  assert.ok(h.events.some(event => event.event === 'batch.operation' && event.state === 'started' && event.index === 3))
})

test('batch preserves the operation failure when release also fails', async () => {
  const operationFailure = Object.assign(new Error('write failed'), { code: 'FILESYSTEM_ERROR' })
  const h = harness({
    python: async () => { throw operationFailure },
    async disconnect() { throw Object.assign(new Error('release failed'), { code: 'RELEASE_FAILED' }) },
  })
  const planFile = resolve('plans/failure.json')
  await assert.rejects(runCli(parseCli(['batch', planFile, '--port', 'COM3']), {
    ...h.dependencies,
    async readFile() { return JSON.stringify({ schemaVersion: 1, operations: [{ op: 'python', source: 'pass' }] }) },
  }), failure => {
    assert.equal(failure.code, 'FILESYSTEM_ERROR')
    assert.deepEqual(failure.cleanupError, { code: 'RELEASE_FAILED', message: 'release failed' })
    assert.equal(failure.batchReport.failed.error.code, 'FILESYSTEM_ERROR')
    assert.deepEqual(failure.batchReport.cleanupError, { code: 'RELEASE_FAILED', message: 'release failed' })
    return true
  })
})

test('invalid batch plans fail before connecting and local paths are rooted at the plan directory', async () => {
  const h = harness(), planFile = resolve('plans/run.json')
  const plan = { schemaVersion: 1, operations: [{ op: 'files.write', path: '/data.bin', file: './data.bin' }] }
  await runCli(parseCli(['batch', planFile, '--port', 'COM3']), {
    ...h.dependencies,
    async readFile(path) {
      if (path === planFile) return JSON.stringify(plan)
      assert.equal(path, resolve('plans/data.bin'))
      return new Uint8Array([7])
    },
  })
  assert.equal(h.calls.find(([name]) => name === 'files.write')[1].data[0], 7)
  const invalid = harness()
  await assert.rejects(runCli(parseCli(['batch', planFile, '--port', 'COM3']), {
    ...invalid.dependencies,
    async readFile() { return JSON.stringify({ schemaVersion: 1, operations: [{ op: 'python', source: 'pass', completion: 'start' }] }) },
  }), { code: 'INVALID_PLAN' })
  assert.deepEqual(invalid.calls, [])
})

test('JSON command envelopes stay versioned and do not put binary payloads in result metadata', () => {
  assert.deepEqual(successEnvelope('files read', { path: '/a', output: '/host/a', bytes: 3 }), {
    schemaVersion: 1, ok: true, command: 'files read', result: { path: '/a', output: '/host/a', bytes: 3 },
  })
})

test('machine-readable help advertises the CLI-only device capabilities and contracts', () => {
  assert.deepEqual(parseCli(['--help', '--json']), { help: true, json: true })
  assert.ok(cliCapabilities.commands.python.includes('--source CODE'))
  assert.ok(cliCapabilities.commands.files.includes('read'))
  assert.ok(cliCapabilities.commands.watch.some(value => value.includes('no script launch')))
  assert.equal(cliCapabilities.output.json, 'One schemaVersion 1 envelope on stdout')
  assert.equal(cliCapabilities.limits.localFileBytes, 16 * 1024 * 1024)
  assert.deepEqual(cliCapabilities.exitCodes, { success: 0, failure: 1, argumentOrPlan: 2, sigint: 130, sigterm: 143 })
})

test('watch streams only bounded text/state events, holds one connection, then releases it', async () => {
  const h = harness({ onConnect(device) {
    device.emit('state', { state: 'ready', identity: { board: 'TEST' } })
    device.emit('python.output', 'hello from device\n')
    device.emit('log', 'frame captured')
    device.emit('diagnostic', { event: 'keepalive.warning', code: 'DEVICE_BUSY' })
    device.emit('preview', { data: new Uint8Array([1, 2, 3]), mimeType: 'image/jpeg' })
  } })
  const options = parseCli(['watch', '--duration', '5', '--port', 'COM3'])
  assert.equal(options.duration, 5)
  const result = await runCli(options, h.dependencies)
  assert.deepEqual(result, { durationMs: result.durationMs, eventCount: 4 })
  assert.ok(h.events.some(event => event.event === 'watch.state' && event.data.state === 'ready'))
  assert.ok(h.events.some(event => event.event === 'watch.output' && event.data === 'hello from device\n'))
  assert.ok(h.events.some(event => event.event === 'watch.log' && event.data === 'frame captured'))
  assert.ok(h.events.some(event => event.event === 'watch.diagnostic' && event.data.code === 'DEVICE_BUSY'))
  assert.ok(!h.events.some(event => event.event === 'watch.preview'))
  assert.equal(h.calls.filter(([name]) => name === 'connect').length, 1)
  assert.equal(h.calls.at(-1)[0], 'disconnect')
  assert.deepEqual(successEnvelope('watch', result), {
    schemaVersion: 1, ok: true, command: 'watch', result,
  })
})

test('watch cancellation reports a summary and releases the connection without reconnecting', async () => {
  const controller = new AbortController(), h = harness()
  const running = runCli(parseCli(['watch', '--duration', '30000', '--port', 'COM3']), {
    ...h.dependencies, signal: controller.signal,
  })
  setTimeout(() => controller.abort(Object.assign(new Error('cancelled'), { code: 'CANCELLED' })), 5)
  await assert.rejects(running, failure => {
    assert.equal(failure.code, 'CANCELLED')
    assert.equal(failure.result.cancelled, true)
    assert.ok(failure.result.durationMs < 30000)
    return true
  })
  assert.equal(h.calls.filter(([name]) => name === 'connect').length, 1)
  assert.equal(h.calls.at(-1)[0], 'disconnect')
})

test('watch surfaces a device disconnect as a failure and still releases ownership', async () => {
  const h = harness({ onConnect(device) { device.emit('state', { state: 'ready' }) } })
  const running = runCli(parseCli(['watch', '--duration', '30000', '--port', 'COM3']), h.dependencies)
  setTimeout(() => h.device.emit('state', { state: 'disconnected', reason: 'SESSION_LOST' }), 5)
  await assert.rejects(running, { code: 'SESSION_LOST' })
  assert.equal(h.calls.at(-1)[0], 'disconnect')
})

test('watch requires an explicit positive bounded duration and a device', () => {
  for (const args of [
    ['watch', '--port', 'COM3'],
    ['watch', '--duration', '0', '--port', 'COM3'],
    ['watch', '--duration', '3600001', '--port', 'COM3'],
    ['watch', '--duration', '25'],
  ]) assert.throws(() => parseCli(args))
})


test('Windows serial file listing preserves the device path and Python launcher', async () => {
  const options = parseCli(['files', 'list', '--port', 'COM9', '--path', '/models', '--json', '--python', 'py'])
  const h = harness()
  await runCli(options, h.dependencies)
  assert.equal(h.calls.find(([name]) => name === 'files.list')[1].path, '/models')
  assert.equal(options.python, 'py')
})

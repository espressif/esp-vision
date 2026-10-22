// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { createPythonApi } from '../src/device_python.js'
import { DeviceSession } from '../src/device_session.js'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { EvRepl } from '../src/evrepl.js'

function executionFixture({ short = false, failWrite = false, failStop = false, stalledReset = false, deferExec = false, deferRun = false, deferStart = false, features = ['file.write.binary', 'script.run', 'device.control'] } = {}) {
  const requests = []
  let running = false
  let sentinel = null
  let startMarker = null
  let startMarkerEmitted = false
  let pendingExec = null
  let terminal = ''
  let startAndCompletionInSameFrame = false
  let session
  let resetRequested = false
  let resetPolls = 0
  let uploaded = 0
  const stdout = (payload) => {
    if (startMarker && sentinel?.marker && String(payload).includes(startMarker) && String(payload).includes(sentinel.marker)) {
      startAndCompletionInSameFrame = true
    }
    session.feedBytes(makeEvMuxFrame({
      channel: 'repl.stdout', type: 'data', sessionId: session.sessionId, vmEpoch: session.vmEpoch, payload,
    }))
  }
  const finishFast = () => {
    if (startMarker && !startMarkerEmitted && sentinel) {
      startMarkerEmitted = true
      running = true
      const completionMarker = sentinel.marker
      stdout(`${startMarker}\r\n${completionMarker}\r\n>>> `)
      running = false
      sentinel = null
      startMarker = null
      startMarkerEmitted = false
      return
    }
    finish()
  }
  const enter = () => {
    if (!startMarker || startMarkerEmitted) return
    startMarkerEmitted = true
    running = true
    const split = Math.floor(startMarker.length / 2)
    stdout(startMarker.slice(0, split))
    stdout(`${startMarker.slice(split)}\r`)
    stdout('\n')
  }
  const finish = () => {
    if (startMarker && !startMarkerEmitted) enter()
    running = false
    if (sentinel) {
      stdout(`${sentinel.startMarker ? '' : sentinel.line}${sentinel.marker}\r\n>>> `)
      sentinel = null
      startMarker = null
      startMarkerEmitted = false
    }
  }
  const finishExec = () => {
    if (!pendingExec) return
    const command = pendingExec
    pendingExec = null
    stdout(`${command}>>> `)
  }
  const parser = new EvMuxParser({
    filterFrame: (metadata, bytes) => {
      const text = new TextDecoder().decode(bytes)
      const payload = metadata.encoding === 'binary' ? bytes : metadata.channel.endsWith('.rpc') ? JSON.parse(text) : text
      requests.push({ metadata, payload })
      if (metadata.channel === 'repl.signal') return
      if (metadata.channel === 'repl.stdin') {
        if (text.includes('machine.soft_reset')) {
          resetRequested = true
          return
        }
        const marker = /~~EVSCRIPT-END:[A-Za-z0-9-]+~~/.exec(text)?.[0]
        if (marker) {
          startMarker = /~~EVSCRIPT-START:[A-Za-z0-9-]+~~/.exec(text)?.[0] ?? null
          startMarkerEmitted = false
          sentinel = { line: text.trimEnd(), marker, startMarker }
          if (startMarker) {
            const echoSplit = Math.floor(text.length / 2)
            stdout(text.slice(0, echoSplit))
            stdout(`${text.slice(echoSplit)}\r\n`)
          }
          if (deferRun) {
            if (startMarker && !deferStart) enter()
            running = true
            stdout('')
          } else if (short && startMarker && !deferStart) finishFast()
          else {
            if (startMarker && !deferStart) enter()
            if (short) finish()
          }
        } else if (deferExec && text.startsWith('exec(')) {
          pendingExec = text
        } else {
          stdout(`${text}\r\n>>> `)
        }
        return
      }
      let response = { ok: true }
      let responseEpoch = session.vmEpoch
      if (metadata.method === 'session.keepalive' && resetRequested) {
        resetPolls += 1
        responseEpoch = stalledReset || resetPolls === 1 ? 1 : 2
        response = { ok: true, sessionId: session.sessionId, vmEpoch: responseEpoch,
          vmReady: stalledReset || resetPolls === 1 || resetPolls >= 3 }
      }
      if (metadata.method === 'file.write') {
        if (payload instanceof Uint8Array) {
          uploaded = new DataView(payload.buffer, payload.byteOffset, payload.byteLength).getUint32(16, true) + payload.length - 20
          response = { ok: true, writtenBytes: uploaded }
        } else if (payload.action === 'begin') {
          uploaded = 0; response = { ok: true, uploadId: payload.uploadId, chunkBytes: 16384 }
        } else response = { ok: true, bytes: uploaded, complete: payload.action === 'commit' }
      }
      if (metadata.method === 'file.write' && failWrite) {
        response = { ok: false, error: { code: 'WRITE_FAILED', message: 'simulated write failure' } }
      } else if (metadata.method === 'script.run') {
        running = true
        stdout('streaming output\r\n')
      } else if (metadata.method === 'device.control' && payload.action === 'stop' && failStop) {
        response = { ok: false, error: { code: 'TX_BUSY', message: 'simulated stop failure' } }
      }
      session.feedBytes(makeEvMuxFrame({
        channel: metadata.channel, type: 'rsp', method: metadata.method, seq: metadata.seq,
        sessionId: session.sessionId, vmEpoch: responseEpoch, payload: JSON.stringify(response),
      }))
      if (metadata.method === 'device.control' && payload.action === 'stop' && !failStop) finish()
    },
  })
  session = new DeviceSession({
    transport: { async writeBytes(bytes) { parser.feedBytes(bytes) } },
    EvMuxParser, makeEvMuxFrame, EvRepl, sleep: async () => {},
    handlers: { text: (text) => { terminal += text } },
  })
  session.state = 'ready'
  session.sessionId = 'a'.repeat(32)
  session.vmEpoch = 1
  session.vmReady = true
  session.limits.fileWriteChunkMax = 16384
  session.capabilities = { features }
  return { session, requests, finish, enter, finishExec, get terminal() { return terminal }, get running() { return running },
    get startAndCompletionInSameFrame() { return startAndCompletionInSameFrame } }
}

test('inline source assignment splits at the REPL limit without changing source text', () => {
  const repl = new EvRepl({ writeEvmux: async () => {} })
  const variableName = '__evcore_python_boundary'
  const source = `${'x'.repeat(470)}😀\nprint("done")`
  assert.ok(repl.execInputBytes(source) <= repl.inputMaxBytes)
  const assignments = repl.createInlineSourceAssignments(source, variableName)
  assert.ok(assignments.length > 1, 'the private assignment prefix requires safe chunks at this boundary')
  assert.ok(assignments.every(statement => repl.execInputBytes(statement) <= repl.inputMaxBytes))
  const rebuilt = assignments.map((statement, index) => JSON.parse(statement.slice(variableName.length + (index === 0 ? 1 : 2)))).join('')
  assert.equal(rebuilt, source)
  const command = repl.createInlineScriptRunCommand(variableName)
  assert.ok(new TextEncoder().encode(command).length <= repl.inputMaxBytes)
  assert.match(command, new RegExp(`exec\\(${variableName}\\)`))
  assert.match(command, new RegExp(`del ${variableName}`))
})

test('python.execute small source waits past 30 seconds without requiring device storage', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] })
  const fixture = executionFixture({ deferRun: true, features: [] })
  t.after(() => fixture.session.close())
  let settled = false
  const execution = fixture.session.executePython('while True: pass').then(output => {
    settled = true
    return output
  })
  await new Promise(resolve => setImmediate(resolve))
  assert.equal(fixture.session.runCapture?.timer, null, 'omitted timeout leaves completion pending')
  assert.equal(fixture.session.runCapture?.path, null, 'small source does not create a device file')
  assert.ok(fixture.requests.some(({ metadata, payload }) => metadata.channel === 'repl.stdin' && String(payload).includes('__evcore_python_')))
  assert.ok(!fixture.requests.some(({ metadata }) => ['file.write', 'script.run'].includes(metadata.method)))
  t.mock.timers.tick(31000)
  await Promise.resolve()
  assert.equal(settled, false, 'silent Python still waits beyond the former fixed completion timeout')
  fixture.finish()
  assert.match(await execution, /^\s*$/)
})

test('Python deadlines include REPL setup and cancellation releases API ownership', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] })
  const timed = executionFixture({ deferExec: true })
  t.after(() => timed.session.close())
  const state = {}
  const python = createPythonApi({ session: timed.session, state })
  const execution = python.execute({ source: 'print(42)', timeout: 1000 })
  const timedOut = assert.rejects(execution, { code: 'TIMEOUT' })
  await new Promise(resolve => setImmediate(resolve))
  t.mock.timers.tick(1000)
  await timedOut
  assert.equal(state.operation, null, 'deadline releases the operation lock even during setup')

  const cancelled = executionFixture({ deferRun: true })
  t.after(() => cancelled.session.close())
  const cancelledState = {}
  const cancelledPython = createPythonApi({ session: cancelled.session, state: cancelledState })
  const controller = new AbortController()
  const cancellation = cancelledPython.execute({ source: 'while True: pass', signal: controller.signal })
  const aborted = assert.rejects(cancellation, { name: 'AbortError' })
  await new Promise(resolve => setImmediate(resolve))
  assert.ok(cancelledState.operation, 'the operation remains owned while Python is still running')
  controller.abort()
  await aborted
  await new Promise(resolve => setImmediate(resolve))
  assert.equal(cancelledState.operation, null, 'cancellation releases the operation lock')
  assert.equal(cancelledState.run, null, 'cancellation does not leave a false active-run claim')
  assert.ok(cancelled.requests.some(({ metadata, payload }) => metadata.method === 'device.control' && payload.action === 'stop'),
    'cancellation requests a stop but the API still reports cancellation, not success')
})

test('interactive execution survives the bounded-run deadline, streams output, and completes on shared stop', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] })
  const fixture = executionFixture()
  t.after(() => fixture.session.close())
  const run = await fixture.session.startScript('/main.py', 'while True: pass')
  let completed = false
  run.done.then(() => { completed = true })
  t.mock.timers.tick(20000)
  await Promise.resolve()
  assert.equal(completed, false)
  assert.equal(fixture.running, true)
  assert.match(fixture.terminal, /streaming output/)
  assert.equal(fixture.session.scriptRunSnapshot().output, '', 'interactive terminal output is not stored a second time')
  const beforeDuplicate = fixture.requests.length
  await assert.rejects(fixture.session.startScript('/other.py', 'pass'), /already running/)
  assert.equal(fixture.requests.length, beforeDuplicate, 'duplicate start does not interrupt the existing run')
  await fixture.session.control('stop')
  assert.equal((await run.done).ok, true)
  assert.equal(fixture.session.scriptRunSnapshot(), null)
  assert.ok(!fixture.terminal.includes('~~EVSCRIPT-END'), 'shared completion markers never leak into terminal output')
})

test('startScript waits for the VM-entry marker and ignores the echoed marker in its command', async () => {
  const fixture = executionFixture({ deferStart: true })
  let resolved = false
  const starting = fixture.session.startScript('/main.py', undefined, { inlineCompletion: true }).then(run => {
    resolved = true
    return run
  })
  await new Promise(resolve => setImmediate(resolve))
  assert.equal(resolved, false, 'a successful stdin write is not evidence that MicroPython entered the runner')
  assert.equal(fixture.running, false)
  assert.equal(fixture.session.runCapture?.timer, null, 'interactive start acknowledgement does not create an aggregate run deadline')
  fixture.enter()
  const run = await starting
  assert.equal(fixture.running, true)
  assert.doesNotMatch(fixture.terminal, /~~EVSCRIPT-START:/, 'the private entry marker is filtered from terminal output')
  fixture.finish()
  await run.done
  fixture.session.close()
})

test('startScript entry acknowledgement has a bounded setup wait', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] })
  const fixture = executionFixture({ deferStart: true })
  const starting = fixture.session.startScript('/main.py', undefined, { inlineCompletion: true })
  const timedOut = assert.rejects(starting, { code: 'SCRIPT_START_TIMEOUT' })
  await new Promise(resolve => setImmediate(resolve))
  t.mock.timers.tick(5000)
  await timedOut
  assert.equal(fixture.session.scriptRunSnapshot(), null)
  fixture.session.close()
})

test('cancellation while waiting for VM entry rejects start and releases the run capture', async () => {
  const fixture = executionFixture({ deferStart: true })
  const controller = new AbortController()
  const starting = fixture.session.startScript('/main.py', undefined, {
    inlineCompletion: true,
    signal: controller.signal,
  })
  const cancelled = assert.rejects(starting, { name: 'AbortError' })
  await new Promise(resolve => setImmediate(resolve))
  controller.abort()
  await cancelled
  await new Promise(resolve => setImmediate(resolve))
  assert.equal(fixture.session.scriptRunSnapshot(), null)
  assert.ok(fixture.requests.some(({ metadata, payload }) => metadata.method === 'device.control' && payload.action === 'stop'))
  fixture.session.close()
})

test('closing the transport while waiting for VM entry rejects start and releases the run capture', async () => {
  const fixture = executionFixture({ deferStart: true })
  const starting = fixture.session.startScript('/main.py', undefined, { inlineCompletion: true })
  const disconnected = assert.rejects(starting, /device disconnected/)
  await new Promise(resolve => setImmediate(resolve))
  assert.ok(fixture.session.repl.scriptStartWaiter, 'startup is waiting for the VM-entry acknowledgement')
  fixture.session.close(new Error('device disconnected'))
  await disconnected
  assert.equal(fixture.session.scriptRunSnapshot(), null)
  assert.equal(fixture.session.repl.scriptStartWaiter, null)
  assert.doesNotMatch(fixture.terminal, /~~EVSCRIPT-START:/)
})

test('short scripts can complete in the same frame while startScript is returning', async () => {
  const fixture = executionFixture({ short: true })
  const run = await fixture.session.startScript('/short.py', undefined, { inlineCompletion: true })
  assert.equal(fixture.startAndCompletionInSameFrame, true)
  assert.equal((await run.done).path, '/short.py')
  assert.equal(fixture.session.scriptRunSnapshot(), null)
  assert.doesNotMatch(fixture.terminal, /~~EVSCRIPT-START:/)
  fixture.session.close()
})

test('parallel starts cannot overwrite the shared execution lifecycle during upload', async () => {
  const fixture = executionFixture()
  const starting = fixture.session.startScript('/first.py', 'pass')
  await assert.rejects(fixture.session.startScript('/second.py', 'pass'), /already running/)
  const first = await starting
  assert.equal(fixture.requests.filter(({ metadata }) => metadata.method === 'script.run').length, 1)
  fixture.finish()
  await first.done
  fixture.session.close()
})

test('startup errors reject the start handle without leaving an active run', async () => {
  const fixture = executionFixture({ failWrite: true })
  await assert.rejects(fixture.session.startScript('/main.py', 'pass'), (error) => error.code === 'WRITE_FAILED')
  assert.equal(fixture.session.scriptRunSnapshot(), null)
  assert.ok(!fixture.requests.some(({ metadata }) => metadata.method === 'script.run'))
  fixture.session.close()
})

test('failed stop does not claim execution has finished', async () => {
  const fixture = executionFixture({ failStop: true })
  const run = await fixture.session.startScript('/main.py', 'while True: pass')
  await assert.rejects(fixture.session.control('stop'), (error) => error.code === 'TX_BUSY')
  assert.equal(fixture.running, true)
  assert.notEqual(fixture.session.scriptRunSnapshot(), null)
  fixture.finish()
  await run.done
  fixture.session.close()
})

test('bounded Agent execution still times out and sends a shared interrupt', async () => {
  const fixture = executionFixture()
  let started
  const starting = new Promise(resolve => { started = resolve })
  const done = fixture.session.runScript('/trial.py', 'while True: pass', { timeout: 30, onStarted: started })
  const rejected = assert.rejects(done, (error) => error.code === 'SCRIPT_TIMEOUT')
  await starting
  await rejected
  assert.equal(fixture.session.scriptRunSnapshot(), null)
  await new Promise(resolve => setTimeout(resolve, 0))
  assert.ok(fixture.requests.some(({ metadata, payload }) => metadata.method === 'device.control' && payload.action === 'stop'))
  fixture.session.close()
})

test('VM reset and disconnect settle interactive completion and allow the next VM run', async () => {
  const fixture = executionFixture()
  const run = await fixture.session.startScript('/main.py', 'while True: pass')
  const reset = assert.rejects(run.done, (error) => error.code === 'VM_EPOCH_CHANGED')
  fixture.session.feedBytes(makeEvMuxFrame({
    channel: 'user.rpc', type: 'event', method: 'hello', sessionId: fixture.session.sessionId, vmEpoch: 2,
    payload: JSON.stringify({ ok: true, evMuxVersion: 4, vmEpoch: 2, vmReady: true, session: { id: fixture.session.sessionId, vmEpoch: 2 } }),
  }))
  await reset
  const next = await fixture.session.startScript('/next.py', 'pass')
  const lost = assert.rejects(next.done, /device disconnected/)
  fixture.session.close(new Error('device disconnected'))
  await lost
})

test('reset and bootloader controls use the shared RPC or framed REPL paths', async () => {
  for (const operation of ['hard-reset', 'soft-reset', 'bootloader']) {
    const fixture = executionFixture()
    await fixture.session.control(operation)
    if (operation === 'hard-reset') {
      assert.deepEqual(fixture.requests.map(({ metadata, payload }) => [metadata.method, payload.action]), [['device.control', 'reset']])
    } else {
      assert.equal(fixture.requests[0].metadata.channel, 'repl.signal')
      assert.equal(fixture.requests[1].metadata.channel, 'repl.stdin')
      assert.match(fixture.requests[1].payload, operation === 'soft-reset' ? /machine\.soft_reset/ : /machine\.bootloader/)
    }
    fixture.session.close()
  }
})

test('soft reset waits for a newer ready epoch and an actual REPL barrier', async () => {
  const fixture = executionFixture()
  try {
    const result = await fixture.session.control('soft-reset')
    assert.equal(result.acknowledged, true)
    assert.equal(result.vmEpoch, 2)
    const polls = fixture.requests.filter(({ metadata }) => metadata.method === 'session.keepalive')
    assert.equal(polls.length, 3, 'old ready epoch and new unready epoch are insufficient')
    const barrier = fixture.requests.at(-1)
    assert.equal(barrier.metadata.channel, 'repl.stdin')
    assert.equal(barrier.metadata.vmEpoch, 2)
    assert.match(barrier.payload, /exec\("1"\)/)
  } finally { fixture.session.close() }
})

test('soft reset times out without probing or running in the old epoch', async () => {
  const fixture = executionFixture({ stalledReset: true })
  try {
    await assert.rejects(fixture.session.control('soft-reset', { timeout: 20 }), error => error.code === 'VM_RESET_TIMEOUT')
    assert.equal(fixture.session.vmReady, false)
    assert.equal(fixture.requests.filter(({ metadata }) => metadata.channel === 'repl.stdin').length, 1)
  } finally { fixture.session.close() }
})

test('one lost keepalive acknowledgement retries within the same negotiated lease', async () => {
  const f = executionFixture(), session = f.session
  const scope = { sessionId: session.sessionId, generation: session.sessionGeneration, keepaliveGeneration: session.keepaliveGeneration }
  let calls = 0
  session.rpc = async (method, payload, options) => {
    assert.equal(method, 'session.keepalive'); assert.deepEqual(payload, {}); assert.equal(options.timeout, 3000)
    calls++
    if (calls === 1) throw Object.assign(new Error('lost reply'), { code: 'TIMEOUT' })
    return { ok: true, sessionId: scope.sessionId }
  }
  assert.equal((await session._keepaliveOnce(scope)).sessionId, scope.sessionId)
  assert.equal(calls, 2)
  session.close()
})

test('keepalive retry fails closed after a second timeout and never crosses owners', async () => {
  for (const changeOwner of [false, true]) {
    const f = executionFixture(), session = f.session
    const scope = { sessionId: session.sessionId, generation: session.sessionGeneration, keepaliveGeneration: session.keepaliveGeneration }
    let calls = 0
    session.rpc = async () => {
      calls++
      if (changeOwner) session.sessionId = 'f'.repeat(32)
      throw Object.assign(new Error('no reply'), { code: 'TIMEOUT' })
    }
    await assert.rejects(session._keepaliveOnce(scope), { code: 'TIMEOUT' })
    assert.equal(calls, changeOwner ? 1 : 2)
    session.close()
  }
})

test('short leases and explicit owner errors do not retry keepalive', async () => {
  for (const code of ['TIMEOUT', 'SESSION_REQUIRED']) {
    const f = executionFixture(), session = f.session
    session.leaseMs = 6000
    const scope = { sessionId: session.sessionId, generation: session.sessionGeneration, keepaliveGeneration: session.keepaliveGeneration }
    let calls = 0
    session.rpc = async () => { calls++; throw Object.assign(new Error(code), { code }) }
    await assert.rejects(session._keepaliveOnce(scope), { code })
    assert.equal(calls, 1)
    session.close()
  }
})

test('soft reset total deadline also bounds a stalled REPL barrier', async () => {
  const fixture = executionFixture()
  fixture.session.repl.execPy = () => new Promise(() => {})
  const started = Date.now()
  try {
    await assert.rejects(fixture.session.control('soft-reset', { timeout: 40 }), { code: 'VM_RESET_TIMEOUT' })
    assert.ok(Date.now() - started < 500, 'must not inherit the separate five-second barrier budget')
  } finally { fixture.session.close() }
})

test('soft reset cancellation reaches the REPL barrier', async () => {
  const fixture = executionFixture()
  const controller = new AbortController()
  fixture.session.repl.execPy = (_source, { signal }) => {
    assert.ok(signal)
    queueMicrotask(() => controller.abort())
    return new Promise(() => {})
  }
  try {
    await assert.rejects(fixture.session.control('soft-reset', { signal: controller.signal }), { name: 'AbortError' })
  } finally { fixture.session.close() }
})

test('soft reset deadline includes a stalled initial transport write', async () => {
  const fixture = executionFixture()
  fixture.session.transport.writeBytes = () => new Promise(() => {})
  try {
    await assert.rejects(fixture.session.control('soft-reset', { timeout: 30 }), { code: 'VM_RESET_TIMEOUT' })
    assert.equal(fixture.requests.length, 0)
  } finally { fixture.session.close() }
})

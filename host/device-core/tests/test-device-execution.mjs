// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { DeviceSession } from '../src/device_session.js'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { EvRepl } from '../src/evrepl.js'

function executionFixture({ short = false, failWrite = false, failStop = false, stalledReset = false } = {}) {
  const requests = []
  let running = false
  let sentinel = null
  let terminal = ''
  let session
  let resetRequested = false
  let resetPolls = 0
  const stdout = (payload) => session.feedBytes(makeEvMuxFrame({
    channel: 'repl.stdout', type: 'data', sessionId: session.sessionId, vmEpoch: session.vmEpoch, payload,
  }))
  const finish = () => {
    running = false
    if (sentinel) {
      stdout(`${sentinel.line}${sentinel.marker}\r\n>>> `)
      sentinel = null
    }
  }
  const parser = new EvMuxParser({
    filterFrame: (metadata, bytes) => {
      const text = new TextDecoder().decode(bytes)
      const payload = metadata.channel.endsWith('.rpc') ? JSON.parse(text) : text
      requests.push({ metadata, payload })
      if (metadata.channel === 'repl.signal') return
      if (metadata.channel === 'repl.stdin') {
        if (text.includes('machine.soft_reset')) {
          resetRequested = true
          return
        }
        const match = /print\("(~~EVSCRIPT-END:[^"]+)"\)/.exec(text)
        if (match) {
          sentinel = { line: text, marker: match[1] }
          if (short) finish()
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
      if (metadata.method === 'script.write' && failWrite) {
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
  session.capabilities = { features: ['script.write', 'script.run', 'device.control'] }
  return { session, requests, finish, get terminal() { return terminal }, get running() { return running } }
}

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

test('short scripts can complete while startScript is returning', async () => {
  const fixture = executionFixture({ short: true })
  const run = await fixture.session.startScript('/short.py', 'print(1)')
  assert.equal((await run.done).path, '/short.py')
  assert.equal(fixture.session.scriptRunSnapshot(), null)
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

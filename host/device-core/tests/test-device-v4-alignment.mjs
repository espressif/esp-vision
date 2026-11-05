// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { Buffer } from 'node:buffer'
import { DeviceSession } from '../src/device_session.js'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { EvRepl, evReplExecInputBytes } from '../src/evrepl.js'

function fixture(options = {}) {
  return new DeviceSession({ transport: { async writeBytes() {} }, EvMuxParser, makeEvMuxFrame, EvRepl, ...options })
}

async function flushMicrotasks() {
  for (let i = 0; i < 20; i++) await Promise.resolve()
}

async function advance(t, ms) {
  t.mock.timers.tick(ms)
  await flushMicrotasks()
}

function startupFixture(t, { signal, progressMs = 12000, totalMs = 12000 } = {}) {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 0 })
  const stats = { writes: 0, requests: 0, disconnects: 0, settled: false, firstRequestAt: null }
  const diagnostics = []
  const session = fixture({
    transport: { async writeBytes() { stats.writes += 1 }, async disconnect() { stats.disconnects += 1 } },
    handlers: { diagnostic: event => diagnostics.push(event) },
  })
  session.parser.setLimits({ frameProgressTimeoutMs: progressMs, frameTotalTimeoutMs: totalMs })
  session.rpc = async () => {
    stats.requests += 1
    stats.firstRequestAt = Date.now()
    throw new Error('first request reached')
  }
  const result = session.negotiate({ role: 'usb', signal }).then(
    () => { throw new Error('unexpected successful fixture negotiation') },
    error => { stats.settled = true; return error },
  )
  t.after(() => { session.close(); session.parser.reset() })
  return { session, stats, diagnostics, result }
}

test('Unicode Python is ASCII on the wire and length preflight includes escapes', async () => {
  let command
  const repl = new EvRepl({ writeEvmux() {} })
  repl._execLine = async line => { command = line; return '' }
  const source = "print('你好😀', '\\u4f60')\n\tpass"
  await repl.execPy(source)
  assert.match(command, /\\u4f60\\u597d\\U0001f600/)
  assert.match(command, /\\\\u4f60/)
  assert.ok([...command].every(char => char.charCodeAt(0) < 128))
  assert.equal(evReplExecInputBytes(source), Buffer.byteLength(command + '\r\n'))
})

test('64 KiB reads use bounded output and serialize with other REPL work', async () => {
  const data = Uint8Array.from({ length: 65536 }, (_, i) => i % 256)
  const repl = new EvRepl({ writeEvmux() {} })
  const counts = []
  repl._execPy = async source => {
    const offset = Number(/f.seek\((\d+)\)/.exec(source)[1])
    const count = Number(/f.read\((\d+)\)/.exec(source)[1])
    counts.push(count)
    await Promise.resolve()
    const chunk = data.slice(offset, offset + count)
    return `__EVREAD_OK__${data.length}:${chunk.length}:` + Buffer.from(chunk).toString('hex') + '\n'
  }
  const reading = repl.readFile('/data.bin', { maxBytes: data.length })
  const after = repl.enqueue(() => { assert.equal(counts.length, 64) })
  assert.deepEqual(await reading, data)
  await after
  assert.ok(counts.every(count => count <= 1024))
  await assert.rejects(repl.readFile('/data.bin', { maxBytes: 1023 }), error => error.code === 'DEVICE_FILE_TOO_LARGE')
})

test('file errors, cancellation and a total deadline stop chunk reads', async () => {
  const repl = new EvRepl({ writeEvmux() {} })
  repl._execPy = async () => '__EVREAD_ERROR__OSError(2)\n'
  await assert.rejects(repl.readFile('/missing'), /OSError\(2\)/)
  await assert.rejects(repl.readFile('/file', { timeout: 0 }), /read timeout/)
  const controller = new AbortController()
  let chunks = 0
  repl._execPy = async () => {
    chunks += 1
    controller.abort(new Error('cancelled'))
    return '__EVREAD_OK__2048:1024:' + '00'.repeat(1024) + '\n'
  }
  await assert.rejects(repl.readFile('/file', { signal: controller.signal }), /cancelled/)
  assert.equal(chunks, 1)
})

test('startup drain parses split frames without adopting old sessions', async () => {
  let session
  const frame = makeEvMuxFrame({ channel: 'user.rpc', type: 'event', method: 'hello', payload: '{}' })
  session = fixture({ sleep: async () => {
    session.feedBytes(frame.slice(0, 20))
    session.feedBytes(frame.slice(20))
  } })
  session.rpc = async () => { throw new Error('first request reached') }
  await assert.rejects(session.negotiate({ drainMs: 300 }), /first request reached/)
  assert.equal(session.parser.getDiagnostics().droppedFrames, 1)
  assert.equal(session.parser.getDiagnostics().bufferedBytes, 0)
  assert.equal(session.sessionId, null)
  session.close()
})

test('startup waits for a valid frame split across 299/301 ms without accepting old sessions', async t => {
  const f = startupFixture(t)
  const frame = makeEvMuxFrame({ channel: 'user.rpc', type: 'event', method: 'hello',
    sessionId: 'a'.repeat(32), vmEpoch: 9,
    payload: JSON.stringify({ evMuxVersion: 4, session: { id: 'a'.repeat(32), vmEpoch: 9 }, vmEpoch: 9 }) })
  await advance(t, 299)
  f.session.feedBytes(frame.subarray(0, 20))
  await advance(t, 1)
  assert.equal(f.stats.settled, false)
  assert.equal(f.stats.requests, 0)
  assert.equal(f.session.draining, true)
  assert.equal(f.session.sessionId, null)
  assert.equal(f.session.vmEpoch, null)
  const snapshot = f.session.parser.getDiagnostics()
  assert.equal(snapshot.partialFrame.startedAt, 299)
  snapshot.partialFrame.totalDeadline = 0
  assert.equal(f.session.parser.getDiagnostics().partialFrame.totalDeadline, 12299)
  await advance(t, 1)
  const staleLog = makeEvMuxFrame({ channel: 'log.idf', payload: 'old startup text' })
  f.session.feedBytes(Buffer.concat([frame.subarray(20), staleLog]))
  assert.match((await f.result).message, /first request reached/)
  assert.equal(f.stats.firstRequestAt, 301)
  assert.equal(f.stats.disconnects, 0)
  assert.equal(f.session.sessionId, null)
  assert.equal(f.session.vmEpoch, null)
  assert.equal(f.session.parser.getDiagnostics().droppedFrames, 2)
  assert.equal(f.session.parser.getDiagnostics().bufferedBytes, 0)
  assert.equal(f.session.startupDrainWait, null)
  assert.equal(f.session.draining, false)
})

test('a frame complete before 300 ms adds no startup wait', async t => {
  const f = startupFixture(t)
  await advance(t, 299)
  f.session.feedBytes(makeEvMuxFrame({ channel: 'log.idf', payload: 'complete' }))
  await advance(t, 1)
  assert.match((await f.result).message, /first request reached/)
  assert.equal(f.stats.firstRequestAt, 300)
  assert.equal(f.session.startupDrainWait, null)
  assert.equal(f.diagnostics.some(event => event.event === 'negotiate.startup_frame_wait'), false)
})

for (const [kind, makePartial] of [
  ['header', frame => frame.subarray(0, 20)],
  ['body', frame => frame.subarray(0, -1)],
]) {
  test(`a truncated startup ${kind} times out before any request, even after the parser empties it`, async t => {
    const f = startupFixture(t, { progressMs: 500, totalMs: 2000 })
    const frame = makeEvMuxFrame({ channel: 'user.rpc', type: 'event', method: 'hello', payload: '{}' })
    await advance(t, 299)
    f.session.feedBytes(makePartial(frame))
    await advance(t, 1)
    assert.equal(f.session.draining, true)
    assert.equal(f.stats.requests, 0)
    await advance(t, 498)
    assert.equal(f.stats.settled, false)
    await advance(t, 1)
    assert.equal((await f.result).code, 'SERIAL_STARTUP_INCOMPLETE_FRAME')
    assert.equal(f.stats.requests, 0)
    assert.equal(f.stats.writes, 0)
    assert.equal(f.stats.disconnects, 1)
    assert.equal(f.session.state, 'disconnected')
    assert.equal(f.session.startupDrainWait, null)
    const error = f.diagnostics.find(event => event.event === 'negotiate.startup_incomplete')
    assert.equal(error.parser.incompleteFrameTimeouts, 1)
    assert.equal(error.parser.bufferedBytes, 0)
  })
}

test('successive coalesced startup frames cannot extend the initial partial-frame deadline', async t => {
  const f = startupFixture(t, { progressMs: 1000, totalMs: 500 })
  const frame = makeEvMuxFrame({ channel: 'log.idf', payload: 'ongoing output' })
  await advance(t, 299)
  f.session.feedBytes(frame.subarray(0, 20))
  await advance(t, 1)
  await advance(t, 1)
  f.session.feedBytes(Buffer.concat([frame.subarray(20), frame.subarray(0, 20)]))
  await advance(t, 299)
  f.session.feedBytes(Buffer.concat([frame.subarray(20), frame.subarray(0, 20)]))
  await advance(t, 198)
  assert.equal(f.stats.settled, false)
  assert.equal(f.session.draining, true)
  assert.equal(f.stats.requests, 0)
  await advance(t, 1)
  assert.equal((await f.result).code, 'SERIAL_STARTUP_INCOMPLETE_FRAME')
  assert.equal(Date.now(), 799)
  assert.equal(f.stats.requests, 0)
  assert.equal(f.stats.writes, 0)
  assert.equal(f.stats.disconnects, 1)
  const error = f.diagnostics.find(event => event.event === 'negotiate.startup_incomplete')
  assert.equal(error.parser.partialFrame.startedAt, 600)
  assert.equal(error.parser.incompleteFrameTimeouts, 0)
})

test('back-to-back adapter notifications cannot send hello between a completed tail and another prefix', async t => {
  const f = startupFixture(t)
  const frame = makeEvMuxFrame({ channel: 'log.idf', payload: 'adjacent chunks' })
  await advance(t, 299)
  f.session.feedBytes(frame.subarray(0, 20))
  await advance(t, 1)
  await advance(t, 1)
  f.session.feedBytes(frame.subarray(20))
  f.session.feedBytes(frame.subarray(0, 20))
  await flushMicrotasks()
  assert.equal(f.stats.requests, 0)
  assert.equal(f.session.draining, true)
  assert.notEqual(f.session.startupDrainWait, null)
  await advance(t, 1)
  f.session.feedBytes(frame.subarray(20))
  assert.match((await f.result).message, /first request reached/)
  assert.equal(f.stats.firstRequestAt, 302)
  assert.equal(f.session.startupDrainWait, null)
})

test('actual startup progress refreshes the idle budget within the fixed total deadline', async t => {
  const f = startupFixture(t, { progressMs: 100, totalMs: 2000 })
  const frame = makeEvMuxFrame({ channel: 'log.idf', payload: 'progress' })
  await advance(t, 299)
  f.session.feedBytes(frame.subarray(0, 20))
  await advance(t, 1)
  await advance(t, 50)
  f.session.feedBytes(frame.subarray(20, 21))
  await advance(t, 99)
  assert.equal(f.stats.settled, false)
  await advance(t, 1)
  assert.equal((await f.result).code, 'SERIAL_STARTUP_INCOMPLETE_FRAME')
  assert.equal(Date.now(), 450)
  assert.equal(f.stats.requests, 0)
  assert.equal(f.stats.disconnects, 1)
})

test('a receive timeout inside the initial 300 ms cannot become an empty successful drain', async t => {
  const f = startupFixture(t, { progressMs: 100, totalMs: 1000 })
  await advance(t, 1)
  f.session.feedBytes(new TextEncoder().encode('\x1eEVMUX/1 h='))
  await advance(t, 100)
  assert.equal(f.session.parser.hasPartialFrame(), false)
  assert.equal(f.session.parser.getDiagnostics().incompleteFrameTimeouts, 1)
  await advance(t, 199)
  assert.equal((await f.result).code, 'SERIAL_STARTUP_INCOMPLETE_FRAME')
  assert.equal(f.stats.requests, 0)
  assert.equal(f.stats.writes, 0)
  assert.equal(f.stats.disconnects, 1)
})

test('a complete tail received after an expired idle budget cannot bypass the queued timeout', async t => {
  const f = startupFixture(t, { progressMs: 100, totalMs: 2000 })
  const frame = makeEvMuxFrame({ channel: 'log.idf', payload: 'late tail' })
  await advance(t, 299)
  f.session.feedBytes(frame.subarray(0, 20))
  await advance(t, 1)
  // Model a busy event loop: the wall clock passes the idle deadline before
  // either timer callback runs, then an already queued read delivers the tail.
  t.mock.timers.setTime(400)
  f.session.feedBytes(frame.subarray(20))
  assert.equal((await f.result).code, 'SERIAL_STARTUP_INCOMPLETE_FRAME')
  assert.equal(f.stats.requests, 0)
  assert.equal(f.stats.writes, 0)
  assert.equal(f.stats.disconnects, 1)
})

test('dripping bytes of one startup frame cannot extend its total budget', async t => {
  const f = startupFixture(t, { progressMs: 150, totalMs: 400 })
  const frame = makeEvMuxFrame({ channel: 'log.idf', payload: 'progress' })
  await advance(t, 299)
  f.session.feedBytes(frame.subarray(0, 20))
  await advance(t, 1)
  for (let i = 0; i < 3; i++) {
    await advance(t, 100)
    f.session.feedBytes(frame.subarray(20 + i, 21 + i))
  }
  await advance(t, 98)
  assert.equal(f.stats.settled, false)
  await advance(t, 1)
  assert.equal((await f.result).code, 'SERIAL_STARTUP_INCOMPLETE_FRAME')
  assert.equal(Date.now(), 699)
  assert.equal(f.stats.requests, 0)
  assert.equal(f.stats.writes, 0)
  assert.equal(f.stats.disconnects, 1)
})

for (const action of ['cancel', 'close']) {
  test(`${action} promptly clears the startup boundary waiter without sending a request`, async t => {
    const controller = new AbortController()
    const f = startupFixture(t, { signal: controller.signal })
    await advance(t, 299)
    f.session.feedBytes(new TextEncoder().encode('\x1eEVMUX/1 h='))
    await advance(t, 1)
    assert.notEqual(f.session.startupDrainWait, null)
    const reason = new Error(`test ${action}`)
    if (action === 'cancel') controller.abort(reason)
    else f.session.close(reason)
    assert.equal(await f.result, reason)
    assert.equal(f.session.startupDrainWait, null)
    assert.equal(f.session.draining, false)
    assert.equal(f.session.negotiateController, null)
    await advance(t, 13000)
    assert.equal(f.stats.requests, 0)
    assert.equal(f.stats.writes, 0)
    assert.equal(f.diagnostics.some(event => event.event === 'negotiate.startup_incomplete'), false)
  })
}

test('injected legacy parsers keep the conservative incomplete-frame failure without private budget methods', async () => {
  class LegacyParser {
    constructor(handlers) { this.parser = new EvMuxParser(handlers) }
    feedBytes(bytes) { this.parser.feedBytes(bytes) }
    hasPartialFrame() { return this.parser.hasPartialFrame() }
    reset() { this.parser.reset() }
  }
  let session, disconnects = 0, requests = 0
  session = fixture({
    EvMuxParser: LegacyParser,
    transport: { async writeBytes() {}, async disconnect() { disconnects += 1 } },
    sleep: async () => session.feedBytes(new TextEncoder().encode('\x1eEVMUX/1 h=')),
  })
  session.rpc = async () => { requests += 1; throw new Error('unexpected request') }
  await assert.rejects(session.negotiate(), error => error.code === 'SERIAL_STARTUP_INCOMPLETE_FRAME')
  assert.equal(requests, 0)
  assert.equal(disconnects, 1)
  assert.equal(session.startupDrainWait, null)
  session.close()
})

test('injected parsers without optional startup diagnostics or boundary methods remain supported', async () => {
  class MinimalParser extends EvMuxParser {
    constructor(handlers) { super(handlers); this.hasPartialFrame = undefined; this.getDiagnostics = undefined }
  }
  const session = fixture({ EvMuxParser: MinimalParser, sleep: async () => {} })
  session.rpc = async () => { throw new Error('first request reached') }
  await assert.rejects(session.negotiate(), /first request reached/)
  session.close()
  session.parser.reset()
})

test('plain console tails do not fail drain and TCP does not wait for drain', async () => {
  for (const role of ['usj', 'tcp']) {
    let sleeps = 0, session
    session = fixture({ sleep: async () => {
      sleeps += 1
      session.feedBytes(new TextEncoder().encode('console\x1eordinary text'))
    } })
    if (role === 'tcp') session.feedBytes(new TextEncoder().encode('\x1eEVMUX/1 h='))
    session.rpc = async () => { throw new Error('first request reached') }
    await assert.rejects(session.negotiate({ role }), /first request reached/)
    assert.equal(sleeps, role === 'tcp' ? 0 : 1)
    session.close()
    session.parser.reset()
  }
})

test('unscoped authorization errors resolve only the matching RPC without adopting state', async () => {
  const session = fixture()
  session.sessionId = 'a'.repeat(32)
  session.vmEpoch = 3
  const pending = { channel: 'debug.rpc', method: 'debug.info', sessionId: session.sessionId, vmEpoch: 3, vmRequest: true }
  session.pendingRpc.set(7, pending)
  const metadata = { seq: 7, channel: 'debug.rpc', method: 'debug.info', type: 'rsp' }
  for (const code of ['NOT_OWNER', 'SESSION_EXPIRED', 'SESSION_REQUIRED']) {
    assert.equal(session._matchesPendingResponse(metadata, { ok: false, error: { code } }), true)
  }
  assert.equal(session._matchesPendingResponse(metadata, { ok: true }), false)
  assert.equal(session._matchesPendingResponse(metadata, { ok: false, error: { code: 'WRITE_FAILED' } }), false)
  assert.equal(session._matchesPendingResponse({ ...metadata, seq: 8 }, { ok: false, error: { code: 'NOT_OWNER' } }), false)
  assert.equal(session.sessionId, 'a'.repeat(32))
  assert.equal(session.vmEpoch, 3)
  session.pendingRpc.clear()
  session.close()
})

test('dropped stdout cannot masquerade as EOF, and changing files fail explicitly', async () => {
  const repl = new EvRepl({ writeEvmux() {} })
  repl._execPy = async () => '__EVREAD_OK__2048:1024:' + '00'.repeat(768) + '\n'
  await assert.rejects(repl.readFile('/file'), error => error.code === 'DEVICE_FILE_READ_INCOMPLETE')
  repl._execPy = async () => '__EVREAD_OK__2048:0:\n'
  await assert.rejects(repl.readFile('/file'), error => error.code === 'DEVICE_FILE_READ_INCOMPLETE')
  let index = 0
  repl._execPy = async () => `__EVREAD_OK__${++index === 1 ? 2048 : 2049}:1024:` + '00'.repeat(1024) + '\n'
  await assert.rejects(repl.readFile('/file'), error => error.code === 'DEVICE_FILE_CHANGED')
})


test('post-acquisition capabilities accepts a response beyond the old four-second deadline', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] })
  let session, waitingForCapabilities = false
  const id = 'a'.repeat(32)
  const caps = {
    ok: true, protocol: { evMuxVersion: 4 },
    features: ['capabilities', 'route.changed', 'framed.repl', 'session.firstCome', 'session.keepalive'],
    channels: ['user.rpc', 'debug.rpc', 'repl.stdin', 'repl.stdout', 'repl.stderr', 'repl.signal', 'log.idf', 'preview.frame'],
    limits: { metadataMax: 384, rxPayloadMax: 2048, replInputMax: 511, vmPendingMax: 1, txLargePayloadMax: 262144, txFrameTimeoutMs: 1500, txQueueTimeoutMs: 2000 },
    session: { policy: 'firstCome', leaseMs: 15000, keepaliveMs: 5000 },
  }
  const reader = new EvMuxParser({ filterFrame: (m, p) => {
    const request = JSON.parse(new TextDecoder().decode(p))
    const acquire = m.method === 'hello' && request.acquire
    const response = m.method === 'capabilities' ? caps : acquire
      ? { ok: true, evMuxVersion: 4, session: { id, vmEpoch: 0, leaseMs: 15000, keepaliveMs: 5000 }, vmReady: false }
      : { ok: true, evMuxVersion: 4, session: null }
    const reply = () => session.feedBytes(makeEvMuxFrame({
      channel: m.channel, type: 'rsp', method: m.method, seq: m.seq,
      ...(acquire ? { sessionId: id, vmEpoch: 0 } : {}), payload: JSON.stringify(response),
    }))
    if (m.method === 'capabilities') {
      waitingForCapabilities = true
      setTimeout(reply, 4500)
    } else reply()
    return false
  } })
  session = fixture({ transport: { async writeBytes(bytes) { reader.feedBytes(bytes) } } })
  t.after(() => session.close())
  const negotiated = session.negotiate({ role: 'tcp', probeRepl: false })
  for (let i = 0; i < 50 && !waitingForCapabilities; i++) await Promise.resolve()
  assert.equal(waitingForCapabilities, true)
  for (let i = 0; i < 10; i++) await Promise.resolve()
  t.mock.timers.tick(4500)
  assert.equal((await negotiated).capabilities.protocol.evMuxVersion, 4)
  assert.equal(session.sessionId, id)
})


test('a TCP frame survives a three-second receive gap and incomplete frames still expire', t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'] })
  let response
  const parser = new EvMuxParser({ rpc: (metadata, payload) => { response = payload } })
  const frame = makeEvMuxFrame({ sid: 'user', channel: 'user.rpc', type: 'rsp', method: 'hello', seq: 1, payload: '{"ok":true}' })
  parser.feedBytes(frame.slice(0, 80))
  t.mock.timers.tick(3500)
  assert.equal(parser.getDiagnostics().droppedFrames, 0)
  parser.feedBytes(frame.slice(80))
  assert.equal(response.ok, true)
  parser.feedBytes(frame.slice(0, 80))
  t.mock.timers.tick(12000)
  assert.equal(parser.getDiagnostics().droppedFrames, 1)
  assert.equal(parser.getDiagnostics().bufferedBytes, 0)
  parser.reset()
})

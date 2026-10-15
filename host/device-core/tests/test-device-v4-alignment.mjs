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

test('unfinished startup frames close the transport before any request', async () => {
  let session, writes = 0, disconnects = 0
  session = fixture({
    transport: { async writeBytes() { writes += 1 }, async disconnect() { disconnects += 1 } },
    sleep: async () => { session.feedBytes(new TextEncoder().encode('\x1eEVMUX/1 h=100 p=0 c=00000000\r\n{')) },
  })
  await assert.rejects(session.negotiate({ drainMs: 300 }), error => error.code === 'SERIAL_STARTUP_INCOMPLETE_FRAME')
  assert.equal(writes, 0)
  assert.equal(disconnects, 1)
  assert.equal(session.state, 'disconnected')
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

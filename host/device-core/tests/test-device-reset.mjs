// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { DeviceSession } from '../src/device_session.js'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { EvRepl } from '../src/evrepl.js'

function fixture({ reset = 'ack', release = 'ack' } = {}) {
  let session, respondReset, respondRelease
  const requests = [], owner = 'a'.repeat(32)
  const reply = (metadata, payload, owned = true) => session.feedBytes(makeEvMuxFrame({
    channel: metadata.channel, type: 'rsp', method: metadata.method, seq: metadata.seq,
    ...(owned ? { sessionId: owner, vmEpoch: 1 } : {}), payload: JSON.stringify(payload),
  }))
  const parser = new EvMuxParser({ filterFrame(metadata, bytes) {
    const payload = metadata.channel.endsWith('.rpc') ? JSON.parse(new TextDecoder().decode(bytes)) : null
    requests.push({ metadata, payload })
    if (metadata.method === 'device.control' && payload.action === 'reset') {
      if (['SESSION_REQUIRED', 'SESSION_EXPIRED', 'NOT_OWNER'].includes(reset)) {
        reply(metadata, { ok: false, error: { code: reset, message: 'Session is no longer owned' } }, false)
        return false
      }
      respondReset = () => reply(metadata, reset === 'reject'
        ? { ok: false, error: { code: 'RESET_DENIED', message: 'Reset was rejected' } }
        : { ok: true, action: 'reset' })
      if (reset === 'write-error') throw Object.assign(new Error('Write failed'), { code: 'TRANSPORT_WRITE' })
      if (reset === 'ack' || reset === 'reject') respondReset()
    } else if (metadata.method === 'session.release') {
      respondRelease = () => reply(metadata, { ok: true, accepted: true })
      if (release === 'ack') respondRelease()
    }
    return false
  } })
  session = new DeviceSession({ transport: { async writeBytes(bytes) { parser.feedBytes(bytes) } }, EvMuxParser, makeEvMuxFrame, EvRepl })
  session.state = 'ready'
  session.sessionId = owner
  session.vmEpoch = 1
  session.vmReady = true
  session.capabilities = { features: ['device.control'] }
  return { session, requests, owner, ack: () => respondReset(), ackRelease: () => respondRelease() }
}

test('hard-reset ACK invalidates the old session even while USJ remains open', async t => {
  const f = fixture(); t.after(() => f.session.close())
  f.session._startKeepalive()
  const reset = f.session.control('hard-reset')
  assert.equal(f.session.state, 'resetting')
  assert.equal(f.session.keepaliveTimer, null)
  assert.deepEqual(await reset, { ok: true, action: 'reset' })
  assert.equal(f.session.state, 'disconnected')
  assert.equal(f.session.sessionId, null)
  assert.equal(f.session.keepaliveTimer, null)
  assert.deepEqual(await f.session.release(), { ok: true, accepted: false })
  assert.equal(f.requests.filter(r => r.metadata.method === 'session.release').length, 0)
})

test('reset timeout stays an unknown failure and cannot become a cleanup timeout', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] })
  const f = fixture({ reset: 'silent' }); t.after(() => f.session.close())
  const reset = f.session.control('hard-reset', { timeout: 50 })
  const rejected = assert.rejects(reset, error => error.code === 'TIMEOUT'
    && error.message === 'EV-MUX device.control timeout' && error.reset?.outcome === 'unknown')
  await new Promise(resolve => setImmediate(resolve))
  t.mock.timers.tick(50)
  await rejected
  assert.equal(f.session.sessionId, null)
  await f.session.release()
  assert.equal(f.requests.length, 1, 'an uncertain reset is neither replayed nor followed by stale release')
})

test('transport loss and a partial write retain their original reset failures', async t => {
  for (const mode of ['silent', 'write-error']) {
    const f = fixture({ reset: mode }); t.after(() => f.session.close())
    const result = f.session.control('hard-reset')
    const expected = mode === 'silent' ? 'LINK_LOST' : 'TRANSPORT_WRITE'
    const rejected = assert.rejects(result, error => error.code === expected && error.reset?.outcome === 'unknown')
    if (mode === 'silent') f.session.close(Object.assign(new Error('Link lost'), { code: expected }))
    await rejected
    assert.equal(f.session.sessionId, null)
    await f.session.release()
    assert.equal(f.requests.length, 1)
  }
})

test('pre-cancelled reset leaves the lease live; cancellation after submission invalidates it', async t => {
  for (const before of [true, false]) {
    const f = fixture({ reset: 'silent' }); t.after(() => f.session.close())
    const controller = new AbortController()
    const reason = Object.assign(new Error('Cancelled'), { code: 'CANCELLED' })
    if (before) controller.abort(reason)
    const reset = f.session.control('hard-reset', { signal: controller.signal })
    const rejected = assert.rejects(reset, error => error === reason)
    if (!before) controller.abort(reason)
    await rejected
    assert.equal(f.session.sessionId, before ? f.owner : null)
    assert.equal(f.requests.length, before ? 0 : 1)
  }
})

test('a confirmed reset rejection retains its live lease and normal release behavior', async t => {
  const f = fixture({ reset: 'reject' }); t.after(() => f.session.close())
  await assert.rejects(f.session.control('hard-reset'), { code: 'RESET_DENIED' })
  assert.equal(f.session.state, 'ready')
  assert.equal(f.session.sessionId, f.owner)
  assert.notEqual(f.session.keepaliveTimer, null)
  await f.session.release()
  assert.equal(f.requests.filter(r => r.metadata.method === 'session.release').length, 1)
})

test('an ownership rejection discards the obsolete lease without claiming an unknown reset outcome', async t => {
  for (const code of ['SESSION_REQUIRED', 'SESSION_EXPIRED', 'NOT_OWNER']) {
    const f = fixture({ reset: code }); t.after(() => f.session.close())
    await assert.rejects(f.session.control('hard-reset'), error => error.code === code && !error.reset)
    assert.equal(f.session.state, 'disconnected')
    assert.equal(f.session.sessionId, null)
    await f.session.release()
    assert.equal(f.requests.length, 1)
  }
})

test('ordinary release timeouts and release failures after a rejected reset remain visible', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] })
  for (const rejectedReset of [false, true]) {
    const f = fixture({ reset: 'reject', release: 'silent' }); t.after(() => f.session.close())
    if (rejectedReset) await assert.rejects(f.session.control('hard-reset'), { code: 'RESET_DENIED' })
    const released = f.session.release({ timeout: 50 })
    const rejected = assert.rejects(released, { code: 'TIMEOUT', message: 'EV-MUX session.release timeout' })
    await new Promise(resolve => setImmediate(resolve))
    t.mock.timers.tick(50)
    await rejected
  }
})

test('release joins a pending reset; concurrent commands cannot write during reset', async t => {
  const f = fixture({ reset: 'defer' }); t.after(() => f.session.close())
  const resetting = f.session.control('hard-reset')
  await assert.rejects(f.session.control('hard-reset'), { code: 'DEVICE_RESETTING' })
  await assert.rejects(f.session.control('stop'), { code: 'DEVICE_RESETTING' })
  await assert.rejects(f.session.negotiate({ drainMs: 0 }), { code: 'DEVICE_RESETTING' })
  assert.equal(f.session.state, 'resetting')
  assert.equal(f.requests.length, 1)
  const released = f.session.release()
  f.ack()
  await resetting
  assert.deepEqual(await released, { ok: true, accepted: false })
  assert.equal(f.requests.length, 1)
})

test('accepted reset settles pre-existing RPCs and clears pending timers', async t => {
  const f = fixture(); t.after(() => f.session.close())
  const pending = f.session.rpc('debug.info', { scope: 'transport.stats' })
  const rejected = assert.rejects(pending, { code: 'DEVICE_RESET' })
  await f.session.control('hard-reset')
  await rejected
  assert.equal(f.session.pendingRpc.size, 0)
  assert.equal(f.session.keepaliveTimer, null)
  assert.equal(f.session.resetPromise, null)
})

test('a release already in flight excludes a new destructive reset', async t => {
  const f = fixture({ release: 'defer' }); t.after(() => f.session.close())
  const releasing = f.session.release()
  assert.equal(f.session.state, 'ready', 'release has not received its ACK yet')
  await assert.rejects(f.session.control('hard-reset'), { code: 'DEVICE_RELEASING' })
  assert.deepEqual(f.requests.map(r => r.metadata.method), ['session.release'])
  f.ackRelease()
  assert.deepEqual(await releasing, { ok: true, accepted: true })
  assert.equal(f.session.sessionId, null)
  assert.equal(f.session.pendingRpc.size, 0)
})

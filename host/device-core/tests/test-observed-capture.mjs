// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { DeviceSession } from '../src/device_session.js'
import { createDeviceOperations } from '../src/device_operations.js'
import { DeviceActionRegistry } from '../src/device_actions.js'
import { bindSessionDeviceActions } from '../src/device_bindings.js'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { EvRepl } from '../src/evrepl.js'
import { jpegFixture } from './jpeg-fixtures.mjs'

const safeFeature = 'debug.capture_frame.observed.v1'
function fixture({ features = ['debug.capture_frame', safeFeature, 'debug.capture_screen', 'device.control'], cameraError = null, pending = false } = {}) {
  let session
  const requests = [], id = 'a'.repeat(32), state = {}
  const jpeg = jpegFixture('small').data
  const replyImage = (request, { sessionId = request.sessionId, vmEpoch = request.vmEpoch } = {}) => session.feedBytes(makeEvMuxFrame({
    channel: request.channel, type: 'rsp', method: request.method, seq: request.seq, sessionId, vmEpoch,
    contentType: 'image/jpeg', encoding: 'binary', width: 4, height: 3, payload: jpeg,
  }))
  const parser = new EvMuxParser({ filterFrame(metadata, bytes) {
    const payload = JSON.parse(new TextDecoder().decode(bytes))
    requests.push({ ...metadata, payload })
    if (metadata.method === 'debug.capture_frame' && pending) return false
    if (metadata.method === 'debug.capture_screen' || (metadata.method === 'debug.capture_frame' && !cameraError)) replyImage(metadata)
    else session.feedBytes(makeEvMuxFrame({ channel: metadata.channel, type: 'rsp', method: metadata.method, seq: metadata.seq,
      sessionId: metadata.sessionId, vmEpoch: metadata.vmEpoch ?? session.vmEpoch,
      payload: JSON.stringify(cameraError && metadata.method === 'debug.capture_frame'
        ? { ok: false, error: { code: cameraError, message: 'No safe camera observation is available' } } : { ok: true }) }))
    return false
  } })
  session = new DeviceSession({ transport: { async writeBytes(bytes) { parser.feedBytes(bytes) } }, EvMuxParser, makeEvMuxFrame, EvRepl })
  session.state = 'ready'
  session.sessionId = id
  session.vmEpoch = 1
  session.vmReady = true
  session.capabilities = { features }
  return { session, requests, replyImage, state, jpeg, api: createDeviceOperations({ session, state }) }
}

test('old unsafe camera firmware is rejected before public, RPC, raw-frame or action requests are sent', async t => {
  for (const features of [[], ['debug.capture_frame'], [safeFeature]]) {
    const f = fixture({ features }); t.after(() => f.session.close())
    const denied = error => error.code === 'UNSUPPORTED_OPERATION' && /update.*firmware/i.test(error.message)
    await assert.rejects(f.api.capture({ source: 'camera' }), denied)
    await assert.rejects(f.session.captureImage('debug.capture_frame'), denied)
    await assert.rejects(f.session.rpc('debug.capture_frame'), denied)
    await assert.rejects(f.session.writeFrame('debug.rpc', 'req', 'debug.capture_frame', '{}'), denied)
    const registry = new DeviceActionRegistry()
    bindSessionDeviceActions(registry)
    await assert.rejects(registry.handler('frame.capture')({ timeoutMs: 1000 }, { session: f.session }), denied)
    assert.deepEqual(f.requests, [])
    assert.equal(f.session.pendingRpc.size, 0)
    assert.equal(f.state.operation, undefined)
  }
})

test('negotiated camera observation returns correlated JPEG bytes and forwards quality', async t => {
  const f = fixture(); t.after(() => f.session.close())
  const image = await f.api.capture({ source: 'camera', quality: 73 })
  assert.deepEqual(image.data, f.jpeg)
  assert.equal(image.mimeType, 'image/jpeg')
  assert.equal(image.metadata.width, 4)
  assert.equal(image.metadata.height, 3)
  assert.equal(f.requests.length, 1)
  assert.equal(f.requests[0].method, 'debug.capture_frame')
  assert.deepEqual(f.requests[0].payload, { quality: 73 })
  assert.equal(f.state.operation, null)
})

test('no published camera frame and observer allocation failures retain explicit firmware errors', async t => {
  for (const cameraError of ['CAMERA_FRAME_NOT_READY', 'NO_MEM']) {
    const f = fixture({ cameraError }); t.after(() => f.session.close())
    await assert.rejects(f.api.capture({ source: 'camera' }), { code: cameraError })
    assert.equal(f.requests.length, 1, 'no sensor initialization, retry or capture fallback is synthesized')
    assert.equal(f.session.lastDebugImage, null)
    assert.equal(f.state.operation, null)
  }
})

test('screen capture retains its independent capability without a camera observation feature', async t => {
  const f = fixture({ features: ['debug.capture_frame', 'debug.capture_screen'] }); t.after(() => f.session.close())
  assert.deepEqual((await f.api.capture({ source: 'screen' })).data, f.jpeg)
  await assert.rejects(f.api.capture({ source: 'camera' }), { code: 'UNSUPPORTED_OPERATION' })
  assert.deepEqual(f.requests.map(request => request.method), ['debug.capture_screen'])
})

test('camera cancellation and timeout release the operation and ignore stale binary responses', async t => {
  const f = fixture({ pending: true }); t.after(() => f.session.close())
  const controller = new AbortController()
  const capture = f.api.capture({ source: 'camera', signal: controller.signal })
  const cancelled = assert.rejects(capture, { name: 'AbortError' })
  await new Promise(setImmediate)
  controller.abort()
  await cancelled
  assert.equal(f.state.operation, null)
  assert.equal(f.session.pendingRpc.size, 0)
  f.replyImage(f.requests[0])
  assert.deepEqual((await f.api.capture({ source: 'screen' })).data, f.jpeg)
  await assert.rejects(f.api.capture({ source: 'camera', timeout: 20 }), { code: 'TIMEOUT' })
  assert.equal(f.state.operation, null)
  assert.equal(f.session.pendingRpc.size, 0)
})

test('soft-reset epochs invalidate a pending camera observation and reject the old image', async t => {
  const f = fixture({ pending: true }); t.after(() => f.session.close())
  const before = f.api.capture({ source: 'camera' })
  const invalidated = assert.rejects(before, { code: 'VM_EPOCH_CHANGED' })
  await new Promise(setImmediate)
  f.session.feedBytes(makeEvMuxFrame({ channel: 'user.rpc', type: 'event', method: 'hello', sessionId: f.session.sessionId, vmEpoch: 2,
    payload: JSON.stringify({ evMuxVersion: 4, session: { id: f.session.sessionId, vmEpoch: 2 }, vmEpoch: 2, vmReady: true }) }))
  await invalidated
  f.replyImage(f.requests[0])
  assert.equal(f.session.vmEpoch, 2)
  assert.deepEqual((await f.api.capture({ source: 'screen' })).data, f.jpeg)
  assert.equal(f.requests.at(-1).vmEpoch, 2)
})

test('acknowledged hard reset discards a pending safe camera response and its old session', async t => {
  const f = fixture({ pending: true }); t.after(() => f.session.close())
  const capture = f.session.captureImage('debug.capture_frame')
  const invalidated = assert.rejects(capture, { code: 'DEVICE_RESET' })
  await new Promise(setImmediate)
  await f.session.control('hard-reset')
  await invalidated
  f.replyImage(f.requests[0])
  assert.equal(f.session.state, 'disconnected')
  assert.equal(f.session.sessionId, null)
  assert.equal(f.session.pendingRpc.size, 0)
})

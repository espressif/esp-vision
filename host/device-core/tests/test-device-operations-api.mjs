// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { createDeviceOperations } from '../src/device_operations.js'

function fixture({ enabled = true, features = ['network.background', 'network.configure', 'debug.capture_frame', 'device.control', 'storage.writer.lease'] } = {}) {
  const calls = []
  let operation = null
  let sequence = 0
  const state = {}
  const session = {
    identity: { deviceId: '123', hostname: 'esp-vision-test' }, capabilities: { features }, vmReady: true, vmEpoch: 1,
    hasFeature: feature => features.includes(feature), snapshot: () => ({ state: 'ready' }),
    async networkStatus(opts) { calls.push(['status', opts]); return { enabled, operation } },
    async configureNetwork(credentials, opts) { calls.push(['configure', credentials, opts]); enabled = true; operation = { id: String(++sequence), state: 'succeeded' }; return { accepted: true, operationId: operation.id } },
    async setNetworkEnabled(value, opts) { calls.push(['enable', value, opts]); enabled = value; operation = { id: String(++sequence), state: 'succeeded' }; return { accepted: true, operationId: operation.id } },
    async forgetNetwork(opts) { calls.push(['forget', opts]); enabled = false; operation = { id: String(++sequence), state: 'succeeded' }; return { accepted: true, operationId: operation.id } },
    async rpc(method, payload, opts) { calls.push(['rpc', method, payload, opts]); return { ok: true } },
    async control(command, opts) { calls.push(['control', command, opts]); return { ok: true } },
    async captureImage(method, payload, opts) { calls.push(['capture', method, payload, opts]); return { data: new Uint8Array([255, 216]), mimeType: 'image/jpeg', metadata: { seq: 1 } } },
  }
  return { session, state, calls, api: createDeviceOperations({ session, state }) }
}

test('inspect returns only requested fields and never invokes REPL', async () => {
  const { api, state, calls, session } = fixture()
  state.run = { path: '/app.py', completion: 'start', status: 'running', done: Promise.resolve() }
  const result = await api.inspect({ fields: ['identity', 'runtime', 'storage'] })
  assert.deepEqual(Object.keys(result), ['identity', 'runtime', 'storage'])
  assert.equal(result.runtime.run.status, 'running')
  assert.equal(result.runtime.run.done, undefined)
  result.identity.hostname = 'changed'
  assert.equal(session.identity.hostname, 'esp-vision-test')
  assert.deepEqual(calls.map(call => call[2]?.volume), ['flash', 'sd'])
  await assert.rejects(api.inspect({ fields: ['unknown'] }), /fields/)
})

test('credentials update preserves omitted enabled state after asynchronous operation completes', async () => {
  const { api, calls, state } = fixture({ enabled: false })
  const result = await api.configure({ network: { credentials: { ssid: 'test', password: '' } } })
  assert.equal(result.network.enabled, false)
  assert.deepEqual(result.completed.map(step => step.action), ['network.configure', 'network.enable'])
  assert.deepEqual(calls.filter(call => call[0] === 'enable').map(call => call[1]), [false])
  assert.equal(state.operation, null)
})

test('credential clearing disables networking; enabling preserves credentials', async () => {
  const { api, calls } = fixture()
  const result = await api.configure({ network: { credentials: null } })
  assert.equal(result.network.enabled, false)
  assert.equal(calls.filter(call => call[0] === 'forget').length, 1)
  await api.configure({ network: { enabled: true } })
  assert.equal(calls.filter(call => call[0] === 'configure').length, 0)
})

test('configuration rejects unknown, contradictory and invalid inputs before mutation', async () => {
  const { api, calls } = fixture()
  for (const request of [
    { other: true }, { network: {} }, { network: { enabled: 'yes' } },
    { network: { credentials: null, enabled: true } },
    { network: { credentials: { ssid: '', password: '' } } },
    { network: { credentials: { ssid: 'a', password: '', hidden: true } } },
    { network: { credentials: { ssid: '你'.repeat(11), password: '' } } },
  ]) await assert.rejects(api.configure(request))
  assert.equal(calls.length, 0)
})

test('configuration failure reports completed steps without pretending to roll back credentials', async () => {
  const { api, session, state } = fixture({ enabled: false })
  session.setNetworkEnabled = async () => { throw Object.assign(new Error('enable failed'), { code: 'OFFLINE' }) }
  await assert.rejects(api.configure({ network: { credentials: { ssid: 'test', password: 'secret12' } } }), error => {
    assert.equal(error.code, 'OFFLINE')
    assert.deepEqual(error.completed.map(step => step.action), ['network.configure'])
    assert.ok(!JSON.stringify(error).includes('secret12'))
    return true
  })
  assert.equal(state.operation, null)
})

test('configuration cancellation records accepted pending operation and releases local lock', async () => {
  const { api, session, state } = fixture()
  const controller = new AbortController()
  session.setNetworkEnabled = async () => { controller.abort(new Error('cancelled')); return { accepted: true, operationId: 'pending' } }
  await assert.rejects(api.configure({ network: { enabled: false }, signal: controller.signal }), error => {
    assert.match(error.message, /cancelled/)
    assert.equal(error.pending.operationId, 'pending')
    assert.deepEqual(error.completed, [])
    return true
  })
  assert.equal(state.operation, null)
})

test('control maps explicit commands, validates mode and can interrupt Python but not file writes', async () => {
  const { api, calls, state } = fixture()
  state.operation = { kind: 'python.execute' }
  await api.control({ action: 'stop' })
  await api.control({ action: 'reset', mode: 'soft' })
  await api.control({ action: 'reset', mode: 'hard' })
  await api.control({ action: 'bootloader' })
  assert.deepEqual(calls.map(call => call[0] === 'rpc' ? call[2].action : call[1]), ['stop', 'soft-reset', 'hard-reset', 'bootloader'])
  state.operation = { kind: 'files.write' }
  await assert.rejects(api.control({ action: 'reset', mode: 'hard' }), error => error.code === 'DEVICE_BUSY')
  await assert.rejects(api.control({ action: 'reset' }), /mode/)
  await assert.rejects(api.control({ action: 'stop', mode: 'hard' }), /mode/)
})

test('capture returns binary JPEG, forwards quality and never substitutes camera for unsupported screen', async () => {
  const { api, calls, state } = fixture()
  const image = await api.capture({ source: 'camera', format: 'jpeg', quality: 80 })
  assert.deepEqual(image.data, new Uint8Array([255, 216]))
  assert.equal(calls[0][1], 'debug.capture_frame')
  assert.deepEqual(calls[0][2], { quality: 80 })
  await assert.rejects(api.capture({ source: 'screen' }), error => error.code === 'UNSUPPORTED_OPERATION')
  await assert.rejects(api.capture({ source: 'camera', quality: 101 }), /quality/)
  await assert.rejects(api.capture({ source: 'camera', format: 'png' }), /jpeg/)
  assert.equal(calls.length, 1)
  assert.equal(state.operation, null)
})

test('capture errors release operation lock and pre-abort performs no IO', async () => {
  const { api, session, state, calls } = fixture()
  session.captureImage = async () => { throw Object.assign(new Error('camera failed'), { code: 'CAPTURE_FAILED' }) }
  await assert.rejects(api.capture({ source: 'camera' }), error => error.code === 'CAPTURE_FAILED')
  assert.equal(state.operation, null)
  const controller = new AbortController(); controller.abort()
  await assert.rejects(api.capture({ source: 'camera', signal: controller.signal }), error => error.name === 'AbortError')
  assert.equal(calls.length, 0)
})


test('configuration accepts firmware-supported PSKs and rejects invalid passwords locally', async () => {
  const { api, calls } = fixture()
  for (const password of ['short', '你'.repeat(8), 'g'.repeat(64)]) {
    await assert.rejects(api.configure({ network: { credentials: { ssid: 'test', password } } }), /password/)
  }
  assert.equal(calls.length, 0)
  await api.configure({ network: { credentials: { ssid: 'test', password: 'a'.repeat(64) } } })
  assert.equal(calls.filter(call => call[0] === 'configure').length, 1)
})

test('accepted asynchronous failure preserves operation identity and never applies later steps', async () => {
  const { api, session, calls, state } = fixture({ enabled: false })
  let polls = 0
  session.networkStatus = async () => ++polls === 1 ? { enabled: false } : {
    enabled: false, operation: { id: '1', state: 'failed', error: { code: 'AUTH_FAILED', message: 'Authentication failed' } },
  }
  await assert.rejects(api.configure({ network: { credentials: { ssid: 'test', password: 'password' }, enabled: true } }), error => {
    assert.equal(error.code, 'AUTH_FAILED')
    assert.deepEqual(error.completed, [])
    assert.equal(error.pending.operationId, '1')
    return true
  })
  assert.equal(calls.filter(call => call[0] === 'enable').length, 0)
  assert.equal(state.operation, null)
})

test('capture and configuration honor a busy device without transmitting requests', async () => {
  const { api, state, calls } = fixture()
  state.operation = { kind: 'files.write' }
  await assert.rejects(api.capture({ source: 'camera' }), error => error.code === 'DEVICE_BUSY')
  await assert.rejects(api.configure({ network: { enabled: false } }), error => error.code === 'DEVICE_BUSY')
  assert.equal(calls.length, 0)
})


test('control reserves the device until acknowledgement so file work cannot race a reset', async () => {
  const { api, session, state } = fixture()
  let release
  session.control = () => new Promise(resolve => { release = resolve })
  const resetting = api.control({ action: 'reset', mode: 'hard' })
  assert.equal(state.operation.kind, 'control')
  await assert.rejects(api.capture({ source: 'camera' }), error => error.code === 'DEVICE_BUSY')
  release({ ok: true })
  await resetting
  assert.equal(state.operation, null)
})

test('configuration snapshots credentials and enabled before waiting for device status', async () => {
  const { api, session, calls } = fixture()
  const original = session.networkStatus
  let release
  let first = true
  session.networkStatus = opts => {
    if (!first) return original(opts)
    first = false
    return new Promise(resolve => { release = () => original(opts).then(resolve) })
  }
  const request = { network: { credentials: { ssid: 'initial', password: '' }, enabled: true } }
  const configuring = api.configure(request)
  request.network.credentials.ssid = 'changed'
  request.network.credentials.password = 'bad'
  request.network.enabled = false
  release()
  const result = await configuring
  assert.equal(result.network.enabled, true)
  assert.deepEqual(calls.find(call => call[0] === 'configure')[1], { ssid: 'initial', password: '' })
})


test('malformed network acknowledgements cannot be reported as completed configuration', async () => {
  const { api, session } = fixture()
  for (const response of [{ ok: true }, { accepted: false, operationId: '1' }, { accepted: true }]) {
    session.setNetworkEnabled = async () => response
    await assert.rejects(api.configure({ network: { enabled: false } }), error => {
      assert.equal(error.code, 'INVALID_RESPONSE')
      assert.deepEqual(error.completed, [])
      return true
    })
  }
})

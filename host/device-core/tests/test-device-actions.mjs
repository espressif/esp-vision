#!/usr/bin/env node
/*
 * SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */

import assert from 'node:assert/strict'
import {
  DEVICE_ACTION_DESCRIPTORS,
  DEVICE_ACTION_SCHEMA_VERSION,
  DeviceActionError,
  DeviceActionGateway,
  DeviceActionRegistry,
  DeviceLease,
  OperationCoordinator,
} from '../src/device_actions.js'
import {
  DeviceEvidenceBuffer,
  EPHEMERAL_PYTHON_PATH,
  EphemeralPythonController,
  bindSessionDeviceActions,
} from '../src/device_bindings.js'

function deferred() {
  let resolve
  const promise = new Promise((done) => { resolve = done })
  return { promise, resolve }
}

async function testRegistryContract() {
  const registry = new DeviceActionRegistry()
  assert.equal(registry.schemaVersion, DEVICE_ACTION_SCHEMA_VERSION)
  assert.equal(DEVICE_ACTION_SCHEMA_VERSION, 1)
  assert.deepEqual(
    registry.list().map(({ name }) => name),
    DEVICE_ACTION_DESCRIPTORS.map(({ name }) => name),
  )
  const input = registry.validate('python.execute', { code: 'print(1)' })
  assert.equal(input.timeoutMs, 8000)
  assert.throws(
    () => registry.validate('python.execute', { code: 'print(1)', surprise: true }),
    (error) => error instanceof DeviceActionError && error.code === 'INVALID_INPUT',
  )
  assert.deepEqual(
    registry.validate('deviceFile.remove', { path: '/agent-test.py' }),
    { path: '/agent-test.py', kind: 'file' },
  )
  assert.deepEqual(
    registry.validate('device.control', { operation: 'bootloader' }),
    { operation: 'bootloader', confirmBootloader: false },
  )
  assert.deepEqual(
    registry.validate('python.runEphemeral', { code: 'print(1)' }),
    { code: 'print(1)', maxDurationMs: 180000 },
  )
  assert.match(registry.descriptor('device.rpc').description, /reuse the exact same ID and payload/)
}

async function testNetworkOperationIdReuse() {
  const registry = new DeviceActionRegistry()
  bindSessionDeviceActions(registry)
  const calls = []
  const session = {
    async rpc(method, payload, options) {
      calls.push({ method, payload, options })
      return { ok: true }
    },
  }
  const operationId = '44444444444444444444444444444444'
  const payload = { action: 'network.configure', operationId, ssid: 'mock-network', password: 'mock-secret' }
  const handler = registry.handler('device.rpc')
  await handler({ method: 'device.control', payload, timeoutMs: 1000 }, { session, signal: null })
  await handler({ method: 'device.control', payload, timeoutMs: 1000 }, { session, signal: null })
  assert.deepEqual(calls.map(({ payload: request }) => request.operationId), [operationId, operationId])
  assert.ok(calls.every(({ method, options }) => method === 'device.control' && options.timeout === 1000))
}

function fakeEphemeralSession({ cleanupFails = false } = {}) {
  let current = null
  return {
    repl: {
      async removeFile(path) {
        assert.equal(path, EPHEMERAL_PYTHON_PATH)
        if (cleanupFails) throw new Error('device disconnected before cleanup')
      },
    },
    async stopForRepl() { return true },
    scriptRunSnapshot() {
      return current ? { path: EPHEMERAL_PYTHON_PATH, output: current.output } : null
    },
    runScript(path, code, { signal, onStarted }) {
      assert.equal(path, EPHEMERAL_PYTHON_PATH)
      current = { code, output: '' }
      onStarted()
      return new Promise((resolve, reject) => {
        current.finish = (output = '') => {
          current.output = output
          resolve({ output })
        }
        current.fail = (message) => reject(new Error(message))
        signal.addEventListener('abort', () => {
          const error = new Error('stopped')
          error.name = 'AbortError'
          reject(error)
        }, { once: true })
      })
    },
    appendOutput(value) { current.output += value },
    finish(value) { current.finish(value) },
    fail(value) { current.fail(value) },
  }
}

async function testEphemeralRunLifecycle() {
  const evidence = new DeviceEvidenceBuffer(10)
  const session = fakeEphemeralSession()
  const controller = new EphemeralPythonController(session, { evidence, generation: 'generation-7' })
  const started = await controller.start('print("ready")', { maxDurationMs: 5000 })
  assert.match(started.runId, /^generation-7:/)
  assert.equal(started.status, 'running')
  await assert.rejects(controller.start('print("duplicate")'), /already active/)
  session.appendOutput('ready\n')
  const observed = await controller.observe(started.runId, { waitMs: 0 })
  assert.equal(observed.output, 'ready\n')
  assert.equal(observed.finished, false)
  session.finish('ready\ndone\n')
  const finished = await controller.observe(started.runId, { waitMs: 100 })
  assert.equal(finished.status, 'finished')
  assert.equal(finished.cleaned, true)
  assert.equal(finished.output, 'ready\ndone\n')
  await assert.rejects(controller.observe('generation-6:old', { waitMs: 0 }), /stale/)
  assert.equal(evidence.snapshot().length, 2)
}

async function testEphemeralTimeoutAndDisconnect() {
  const timedSession = fakeEphemeralSession()
  const timedController = new EphemeralPythonController(timedSession, { generation: 'generation-9' })
  const timed = await timedController.start('while True: pass')
  timedSession.fail('script run timeout after 1000ms')
  const timedResult = await timedController.observe(timed.runId, { waitMs: 100 })
  assert.equal(timedResult.status, 'timed-out')
  assert.equal(timedResult.cleaned, true)

  const disconnectedSession = fakeEphemeralSession({ cleanupFails: true })
  const disconnectedController = new EphemeralPythonController(disconnectedSession, { generation: 'generation-10' })
  const disconnected = await disconnectedController.start('print("disconnect")')
  disconnectedSession.fail('serial closed')
  const disconnectedResult = await disconnectedController.observe(disconnected.runId, { waitMs: 100 })
  assert.equal(disconnectedResult.status, 'failed')
  assert.equal(disconnectedResult.cleaned, false)
  assert.equal(disconnectedResult.residualPath, EPHEMERAL_PYTHON_PATH)
}

async function testEphemeralStopAndCleanupFailure() {
  const session = fakeEphemeralSession({ cleanupFails: true })
  const controller = new EphemeralPythonController(session, { generation: 'generation-8' })
  const started = await controller.start('while True: pass')
  const stopped = await controller.stop(started.runId)
  assert.equal(stopped.status, 'stopped')
  assert.equal(stopped.finished, true)
  assert.equal(stopped.cleaned, false)
  assert.equal(stopped.residualPath, EPHEMERAL_PYTHON_PATH)
  assert.match(stopped.cleanupError, /disconnected/)
  await assert.rejects(controller.start('print(2)', { signal: AbortSignal.abort() }), /cancelled|aborted/i)
}

async function testBootloaderAuthorization() {
  const registry = new DeviceActionRegistry()
  const lease = new DeviceLease()
  const token = lease.acquire('agent')
  const session = { state: 'ready' }
  let calls = 0
  registry.bind('device.control', async () => { calls += 1 })
  const gateway = new DeviceActionGateway({
    registry,
    lease,
    getSession: () => session,
    authorize: async (descriptor, input) => (
      descriptor.name !== 'device.control'
      || input.operation !== 'bootloader'
      || input.confirmBootloader === true
    ),
  })
  await assert.rejects(
    gateway.invoke('device.control', { operation: 'bootloader' }, {
      actor: 'agent',
      requestId: 'bootloader-denied',
    }),
    (error) => error.code === 'ACTION_NOT_AUTHORIZED',
  )
  await gateway.invoke('device.control', {
    operation: 'bootloader',
    confirmBootloader: true,
  }, {
    actor: 'agent',
    requestId: 'bootloader-approved',
  })
  assert.equal(calls, 1)
  token.release()
}

async function testLeaseOwnership() {
  const lease = new DeviceLease()
  const first = lease.acquire('agent')
  const second = lease.acquire('agent')
  assert.throws(
    () => lease.acquire('unknown-client'),
    (error) => error.code === 'INVALID_ACTOR',
  )
  lease.assertAccess('agent')
  assert.deepEqual(lease.snapshot().actors, ['agent'])
  assert.equal(lease.snapshot().holders, 2)
  second.release()
  first.release()
  assert.equal(lease.snapshot().state, 'free')
}

async function testCoordinatorFifoAndCancellation() {
  const coordinator = new OperationCoordinator()
  const gate = deferred()
  const order = []
  const first = coordinator.run({
    actor: 'agent',
    requestId: 'agent-active',
    actionName: 'deviceTest.run',
  }, async () => {
    order.push('agent-active')
    await gate.promise
  })
  const second = coordinator.run({
    actor: 'agent',
    requestId: 'agent-queued-1',
    actionName: 'device.inspect',
  }, async () => { order.push('agent-queued-1') })
  const third = coordinator.run({
    actor: 'agent',
    requestId: 'agent-queued-2',
    actionName: 'device.inspect',
  }, async () => { order.push('agent-queued-2') })
  gate.resolve()
  await Promise.all([first, second, third])
  assert.deepEqual(order, ['agent-active', 'agent-queued-1', 'agent-queued-2'])

  let wasAborted = false
  const controller = new AbortController()
  const activeAgent = coordinator.run({
    actor: 'agent',
    requestId: 'cancel-me',
    actionName: 'deviceTest.run',
    signal: controller.signal,
  }, async (signal) => {
    await new Promise((resolve, reject) => {
      signal.addEventListener('abort', () => {
        wasAborted = true
        reject(signal.reason)
      }, { once: true })
    })
  })
  await Promise.resolve()
  controller.abort(new Error('cancelled by test'))
  await assert.rejects(activeAgent)
  assert.equal(wasAborted, true)
  coordinator.close()
}

async function testGateway() {
  const registry = new DeviceActionRegistry()
  const lease = new DeviceLease()
  const token = lease.acquire('agent')
  const session = { state: 'ready', marker: 'session' }
  registry.bind('device.inspect', async (input, context) => ({
    input,
    actor: context.actor,
    session: context.session.marker,
  }))
  const gateway = new DeviceActionGateway({ registry, lease, getSession: () => session })
  const result = await gateway.invoke('device.inspect', { includeFiles: false }, {
    actor: 'agent',
    requestId: 'inspect-1',
  })
  assert.equal(result.actor, 'agent')
  assert.equal(result.input.logLimit, 100)
  assert.equal(result.session, 'session')
  token.release()
  await assert.rejects(
    gateway.invoke('device.inspect', {}, { actor: 'agent', requestId: 'inspect-2' }),
    (error) => error.code === 'DEVICE_LEASE_REQUIRED',
  )
}

async function testBusyRecovery() {
  const registry = new DeviceActionRegistry()
  registry.bind('device.connect', async () => {
    const error = new Error('open failed: Device or resource busy')
    error.code = 'EBUSY'
    throw error
  })
  const gateway = new DeviceActionGateway({ registry })
  await assert.rejects(
    gateway.invoke('device.connect', { port: '/dev/test' }, {
      actor: 'agent',
      requestId: 'busy-connect',
    }),
    (error) => error.code === 'DEVICE_BUSY' && /explicit handoff/.test(error.recovery),
  )
}

async function testPathRecovery() {
  for (const code of ['INVALID_PATH', 'PATH_TOO_LONG']) {
    const registry = new DeviceActionRegistry()
    registry.bind('deviceFile.write', async () => {
      const error = new Error('Device RPC path exceeds the firmware path contract')
      error.code = code
      throw error
    })
    const lease = new DeviceLease()
    const handle = lease.acquire('agent')
    const gateway = new DeviceActionGateway({ registry, lease, getSession: () => ({ state: 'ready' }) })
    await assert.rejects(
      gateway.invoke('deviceFile.write', { path: '/test.py', content: '' }, {
        actor: 'agent', requestId: `path-${code}`,
      }),
      (error) => error.code === code && /path/.test(error.recovery),
    )
    handle.release()
  }
}

async function testFirmwareVersionRecovery() {
  const registry = new DeviceActionRegistry()
  registry.bind('device.connect', async () => {
    const error = new Error('acquire requires evMuxVersion 4')
    error.code = 'VERSION_MISMATCH'
    throw error
  })
  const gateway = new DeviceActionGateway({ registry })
  await assert.rejects(
    gateway.invoke('device.connect', { port: '/dev/test' }, { actor: 'agent', requestId: 'v4-required' }),
    (error) => error.code === 'FIRMWARE_INCOMPATIBLE' && /explicitly supports EV-MUX v4/.test(error.recovery),
  )
}

async function testControlCancellationBinding() {
  const registry = new DeviceActionRegistry()
  bindSessionDeviceActions(registry)
  const lease = new DeviceLease()
  const token = lease.acquire('agent')
  const controller = new AbortController()
  const session = {
    state: 'ready',
    control: async (operation, { signal }) => {
      assert.equal(operation, 'soft-reset')
      assert.ok(signal, 'the shared control binding must forward cancellation')
      return new Promise((_resolve, reject) => {
        signal.addEventListener('abort', () => reject(signal.reason), { once: true })
        queueMicrotask(() => controller.abort())
      })
    },
  }
  const gateway = new DeviceActionGateway({ registry, lease, getSession: () => session })
  try {
    await assert.rejects(gateway.invoke('device.control', { operation: 'soft-reset' }, {
      actor: 'agent', requestId: 'reset-cancelled', signal: controller.signal,
    }), (error) => error.code === 'ACTION_CANCELLED')
  } finally { token.release() }
}

await testRegistryContract()
await testControlCancellationBinding()
await testNetworkOperationIdReuse()
await testBootloaderAuthorization()
await testLeaseOwnership()
await testCoordinatorFifoAndCancellation()
await testGateway()
await testBusyRecovery()
await testPathRecovery()
await testFirmwareVersionRecovery()
await testEphemeralRunLifecycle()
await testEphemeralStopAndCleanupFailure()
await testEphemeralTimeoutAndDisconnect()
console.log('Device action contract tests passed')

// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { prepareNodeRomPort, recoverNodeRomPort } from '../src/node_rom_port.js'

const MAC = 'e8f60ae75642'
const native = (path = '/dev/ttyACM0', extra = {}) => ({ path, vendorId: 0x303a, productId: 0x4003, serialNumber: MAC, location: '1-2:1.0', ...extra })
const rom = (path = '/dev/ttyACM1', extra = {}) => native(path, { productId: 18, location: '1-2', ...extra })
const failure = (code, message = code) => Object.assign(new Error(message), { code })
function fixture(options = {}) {
  let clock = 0, enumerations = 0
  const calls = { connect: [], control: [], python: [], disconnect: 0, creates: 0, realpath: [], enumerate: [] }
  const controller = options.controller ?? new AbortController()
  const connected = { identity: { deviceId: MAC }, capabilities: { sinks: [
    { id: 'usb', kind: 'usb_otg_cdc', owner: true, state: 'active', ready: true },
  ] }, ...options.connected }
  const device = {
    async connect(request) { calls.connect.push(request); if (options.connectError) throw options.connectError; return connected },
    python: { async execute(request) { calls.python.push(request); return options.pythonResult ?? { ok: true, outputTruncated: false, output: `__EV_ROM_ID__${MAC}\n` } } },
    async control(request) {
      calls.control.push(request)
      if (options.control) return options.control(controller)
      if (options.controlError) throw options.controlError
      return { ok: true, operation: 'bootloader', acknowledged: false, outcome: 'unknown' }
    },
    async disconnect() { calls.disconnect += 1; if (options.cleanupError) throw options.cleanupError },
  }
  const snapshots = options.snapshots ?? [[native()], [rom()]]
  const dependencies = {
    now: () => clock,
    async sleep(ms, signal) { clock += ms; if (options.onSleep) options.onSleep(controller, clock); signal?.throwIfAborted() },
    async realpath(path) { calls.realpath.push(path); return options.realpath ? options.realpath(path, clock) : path },
    async listSerial(request) {
      calls.enumerate.push(request)
      if (options.onEnumerate) options.onEnumerate(controller, ++enumerations)
      if (options.enumerationError) throw options.enumerationError
      if (options.enumerationDelay) clock += options.enumerationDelay
      return snapshots[Math.min(enumerations++ , snapshots.length - 1)]
    },
    serialTransport(request) { return request },
    createDevice(request) { calls.creates += 1; calls.transport = request.transport; return device },
  }
  const run = (overrides = {}) => prepareNodeRomPort({ port: '/dev/ttyACM0', timeout: 200, signal: controller.signal, ...overrides }, dependencies)
  const recover = (previous, overrides = {}) => recoverNodeRomPort(previous, { python: 'selected-python', timeout: 200, signal: controller.signal, ...overrides }, dependencies)
  return { calls, run, recover, controller, now: () => clock }
}

test('imports are passive; ROM, USJ, UART and unknown selections never create an EV-MUX session', async () => {
  for (const descriptor of [rom('/dev/ttyACM0'), native('/dev/ttyACM0', { productId: 0x1001 }),
    native('/dev/ttyACM0', { vendorId: 0x10c4, productId: 0xea60 }), native('/dev/ttyACM0', { vendorId: null, productId: null })]) {
    const f = fixture({ snapshots: [[descriptor]] })
    const result = await f.run()
    assert.equal(result.port, '/dev/ttyACM0')
    assert.equal(f.calls.creates, 0)
    assert.equal(f.calls.control.length, 0)
    assert.equal(result.expectedMac, descriptor.productId === 18 ? MAC : null)
  }
})

test('a missing exact by-id path may settle, then existing ROM uses its canonical OS path', async () => {
  const selected = '/dev/serial/by-id/exact-selected'
  const f = fixture({ realpath: (_, time) => { if (time < 100) throw failure('ENOENT'); return '/dev/ttyACM1' }, snapshots: [[rom()]] })
  const result = await f.run({ port: selected })
  assert.equal(result.port, '/dev/ttyACM1')
  assert.equal(result.expectedMac, MAC)
  assert.equal(result.transition.requested, false)
  assert.equal(result.transition.location, '1-2')
  assert.equal(f.calls.creates, 0)
  assert.ok(f.calls.realpath.every(path => path === selected))
})

test('a vanished selected by-id never authorizes filename-based selection of another board', async () => {
  const f = fixture({ realpath: () => { throw failure('ENOENT') }, snapshots: [[rom('/dev/ttyACM8')]] })
  await assert.rejects(f.run({ port: `/dev/serial/by-id/usb-${MAC}` }), { code: 'ROM_PORT_LOST' })
  assert.equal(f.calls.creates, 0)
  assert.equal(f.calls.enumerate.length, 0)
  assert.equal(f.now(), 200)
})

test('verified native runtime transitions once and binds only matching ROM serial plus location', async () => {
  const f = fixture()
  const result = await f.run()
  assert.equal(result.port, '/dev/ttyACM1')
  assert.equal(result.expectedMac, MAC)
  assert.deepEqual(result.transition.command, { attempted: true, acknowledged: false, outcome: 'unknown' })
  assert.equal(result.transition.observed, 'rom')
  assert.equal(result.transition.cleanup.ok, true)
  assert.equal(f.calls.connect.length, 1)
  assert.deepEqual(f.calls.transport, { serial: '/dev/ttyACM0', role: 'usb' })
  assert.equal(f.calls.connect[0].probeRepl, false)
  assert.equal(f.calls.connect[0].autoBaud, false)
  assert.equal(f.calls.connect[0].role, 'usb')
  assert.equal(f.calls.control.length, 1)
  assert.equal(f.calls.disconnect, 1)
  assert.equal(f.calls.python.length, 0)
})

test('legacy null deviceId uses one small public Python unique_id probe before bootloader', async () => {
  const f = fixture({ connected: { identity: { deviceId: null } } })
  const result = await f.run()
  assert.equal(result.transition.identitySource, 'machine.unique_id')
  assert.equal(f.calls.python.length, 1)
  assert.ok(Buffer.byteLength(f.calls.python[0].source) < 511)
  assert.equal(f.calls.python[0].completion, 'wait')
  assert.equal(f.calls.control.length, 1)
})

test('wrong or malformed non-null identity never falls back to Python or resets', async () => {
  for (const id of ['e8f60ae75653', 'unknown']) {
    const f = fixture({ connected: { identity: { deviceId: id } } })
    await assert.rejects(f.run(), { code: 'ROM_RUNTIME_IDENTITY_MISMATCH' })
    assert.equal(f.calls.python.length, 0)
    assert.equal(f.calls.control.length, 0)
    assert.equal(f.calls.disconnect, 1)
  }
})

test('incomplete legacy unique_id output cannot authorize reset', async () => {
  for (const result of [{ ok: true, outputTruncated: true, output: `__EV_ROM_ID__${MAC}\n` },
    { ok: true, outputTruncated: false, output: `__EV_ROM_ID__${MAC}\n__EV_ROM_ID__${MAC}\n` },
    { ok: true, outputTruncated: false, output: 'Traceback\n' }]) {
    const f = fixture({ connected: { identity: { deviceId: null } }, pythonResult: result })
    await assert.rejects(f.run(), { code: 'ROM_RUNTIME_IDENTITY_UNAVAILABLE' })
    assert.equal(f.calls.control.length, 0)
    assert.equal(f.calls.disconnect, 1)
  }
})

test('missing serial or location and ambiguous selected descriptors fail without opening a device', async () => {
  for (const extra of [{ serialNumber: null }, { serialNumber: 'non-MAC' }, { location: null }]) {
    const f = fixture({ snapshots: [[native('/dev/ttyACM0', extra)]] })
    await assert.rejects(f.run(), { code: 'ROM_PORT_IDENTITY_REQUIRED' })
    assert.equal(f.calls.creates, 0)
  }
  const f = fixture({ snapshots: [[native(), native()]] })
  await assert.rejects(f.run(), { code: 'ROM_PORT_AMBIGUOUS' })
  assert.equal(f.calls.creates, 0)
})

test('an inactive USB owner or different owner cannot authorize bootloader', async () => {
  for (const sinks of [[{ id: 'usb', kind: 'usb_otg_cdc', owner: true, state: 'idle', ready: true }],
    [{ id: 'usj', kind: 'usb_serial_jtag', owner: true, state: 'active', ready: true }]]) {
    const f = fixture({ connected: { capabilities: { sinks } } })
    await assert.rejects(f.run(), { code: 'ROM_RUNTIME_INTERFACE_MISMATCH' })
    assert.equal(f.calls.control.length, 0)
    assert.equal(f.calls.disconnect, 1)
  }
})

test('other MACs or physical locations never replace the selected device', async () => {
  const f = fixture({ snapshots: [[native()], [rom('/dev/ttyACM1', { serialNumber: 'e8f60ae75653' }), rom('/dev/ttyACM2', { location: '1-3' })]] })
  await assert.rejects(f.run(), { code: 'ROM_PORT_REBIND_TIMEOUT' })
  assert.equal(f.calls.creates, 1)
  assert.equal(f.calls.control.length, 1)
  assert.equal(f.calls.disconnect, 1)
  assert.equal(f.now(), 200)
})

test('two matching ROM interfaces are ambiguous and never selected', async () => {
  const f = fixture({ snapshots: [[native()], [rom('/dev/ttyACM1'), rom('/dev/ttyACM2')]] })
  await assert.rejects(f.run(), { code: 'ROM_PORT_AMBIGUOUS' })
  assert.equal(f.calls.control.length, 1)
})

test('unknown command outcome and cleanup failure remain separate from observed ROM', async () => {
  const controlError = Object.assign(failure('TIMEOUT'), { reset: { outcome: 'unknown', acknowledged: false } })
  const f = fixture({ controlError, cleanupError: failure('SESSION_RELEASE_TIMEOUT') })
  const result = await f.run()
  assert.equal(result.transition.observed, 'rom')
  assert.equal(result.transition.command.acknowledged, false)
  assert.equal(result.transition.command.outcome, 'unknown')
  assert.equal(result.transition.command.error.code, 'TIMEOUT')
  assert.equal(result.transition.cleanup.ok, false)
  assert.equal(result.transition.cleanup.error.code, 'SESSION_RELEASE_TIMEOUT')
  assert.equal(f.calls.control.length, 1)
})

test('ordinary handshake or command errors are returned without bootloader retry', async () => {
  const error = failure('SESSION_BUSY')
  const f = fixture({ connectError: error })
  await assert.rejects(f.run(), value => value === error)
  assert.equal(f.calls.control.length, 0)
  assert.equal(f.calls.disconnect, 1)
  const other = fixture({ controlError: failure('TIMEOUT') })
  await assert.rejects(other.run(), { code: 'TIMEOUT' })
  assert.equal(other.calls.control.length, 1)
  assert.equal(other.calls.enumerate.length, 1)
})

test('Windows COM aliases and pyserial interface suffixes bind the same physical USB port', async () => {
  const f = fixture({ snapshots: [[native('COM8', { location: '1-5:x.0' })], [rom('COM9', { location: '1-5' })]] })
  const result = await f.run({ port: '\\\\.\\com8' })
  assert.equal(result.port, 'COM9')
  assert.equal(result.expectedMac, MAC)
  assert.equal(f.calls.realpath.length, 0)
})

test('enumeration finishing after the deadline does not accept a late ROM match', async () => {
  const f = fixture({ enumerationDelay: 250 })
  await assert.rejects(f.run(), { code: 'ROM_PORT_REBIND_TIMEOUT' })
  assert.equal(f.calls.control.length, 1)
})

test('cancel before entry, during command, and during rebind never replays control', async () => {
  const reason = Object.assign(new Error('cancelled'), { name: 'AbortError' })
  const before = fixture(); before.controller.abort(reason)
  await assert.rejects(before.run(), value => value === reason)
  assert.equal(before.calls.creates, 0)
  const during = fixture({ control(controller) { controller.abort(reason); throw reason } })
  await assert.rejects(during.run(), value => value === reason)
  assert.equal(during.calls.control.length, 1)
  assert.equal(during.calls.disconnect, 1)
  const poll = fixture({ snapshots: [[native()], []], onSleep(controller) { controller.abort(reason) } })
  await assert.rejects(poll.run(), value => value === reason)
  assert.equal(poll.calls.control.length, 1)
  assert.equal(poll.calls.disconnect, 1)
})

test('unavailable descriptors leave the exact canonical selection untouched and unprobed', async () => {
  const f = fixture({ enumerationError: failure('SERIAL_UNAVAILABLE'), realpath: () => '/dev/ttyACM7' })
  const result = await f.run({ port: '/dev/serial/by-id/exact' })
  assert.deepEqual(result, { port: '/dev/ttyACM7', expectedMac: null, transition: null })
  assert.equal(f.calls.creates, 0)
})


const previousRom = (extra = {}) => ({ port: '/dev/ttyACM0', expectedMac: MAC,
  transition: { observed: 'rom', requested: false, serial: MAC, location: '1-2' }, ...extra })

test('confirmed UVC runtime PID4023 uses the same identity-verified one-shot handoff', async () => {
  const f = fixture({ snapshots: [[native('/dev/ttyACM0', { productId: 0x4023 })], [rom()]] })
  const result = await f.run()
  assert.equal(result.expectedMac, MAC)
  assert.equal(result.transition.observed, 'rom')
  assert.equal(f.calls.control.length, 1)
  const other = fixture({ snapshots: [[native('/dev/ttyACM0', { productId: 0x4021 })]] })
  assert.equal((await other.run()).transition, null)
  assert.equal(other.calls.creates, 0)
})

test('read-only initialization recovery verifies same-board runtime and reuses production handoff once', async () => {
  for (const productId of [0x4003, 0x4023]) {
    const runtime = native('/dev/ttyACM2', { productId })
    const f = fixture({ snapshots: [[runtime], [runtime], [rom('/dev/ttyACM3')]] })
    const result = await f.recover(previousRom())
    assert.equal(result.port, '/dev/ttyACM3')
    assert.equal(result.expectedMac, MAC)
    assert.deepEqual(result.transition.recovery, { previousPort: '/dev/ttyACM0', runtimePid: productId })
    assert.equal(f.calls.transport.serial, '/dev/ttyACM2')
    assert.equal(f.calls.control.length, 1)
    assert.equal(f.calls.disconnect, 1)
  }
})

test('recovery never repeats a previous active handoff or uses an incomplete previous binding', async () => {
  for (const previous of [previousRom({ expectedMac: 'unknown' }), previousRom({ transition: null }),
    previousRom({ transition: { observed: 'rom', requested: true, serial: MAC, location: '1-2' } }),
    previousRom({ transition: { observed: 'rom', requested: false, serial: 'e8f60ae75653', location: '1-2' } }),
    previousRom({ transition: { observed: 'rom', requested: false, serial: MAC, location: null } })]) {
    const f = fixture()
    assert.equal(await f.recover(previous), null)
    assert.equal(f.calls.enumerate.length, 0)
    assert.equal(f.calls.creates, 0)
  }
})

test('persistent ROM or absence exhausts only passive observation before returning null', async () => {
  for (const snapshot of [[rom('/dev/ttyACM1')], [], [native('/dev/ttyACM8', { serialNumber: 'e8f60ae75653', location: '2-3' })]]) {
    const f = fixture({ snapshots: [snapshot] })
    assert.equal(await f.recover(previousRom()), null)
    assert.equal(f.now(), 200)
    assert.equal(f.calls.enumerate.length, 4)
    assert.equal(f.calls.creates, 0)
    assert.equal(f.calls.control.length, 0)
  }
})

test('recovery rejects wrong MAC, moved location and ambiguous matching runtimes', async () => {
  for (const runtime of [native('/dev/ttyACM2', { serialNumber: 'e8f60ae75653' }), native('/dev/ttyACM2', { location: '2-3' })]) {
    const f = fixture({ snapshots: [[runtime]] })
    await assert.rejects(f.recover(previousRom()), { code: 'ROM_RUNTIME_IDENTITY_MISMATCH' })
    assert.equal(f.calls.creates, 0)
  }
  const f = fixture({ snapshots: [[native('/dev/ttyACM2'), native('/dev/ttyACM3')]] })
  await assert.rejects(f.recover(previousRom()), { code: 'ROM_PORT_AMBIGUOUS' })
  assert.equal(f.calls.creates, 0)
})

test('recovery rechecks identity before connecting when a selected OS path was reused', async () => {
  const correct = native('/dev/ttyACM2')
  const wrong = native('/dev/ttyACM2', { serialNumber: 'e8f60ae75653' })
  const f = fixture({ snapshots: [[correct], [wrong]] })
  await assert.rejects(f.recover(previousRom()), { code: 'ROM_RUNTIME_IDENTITY_MISMATCH' })
  assert.equal(f.calls.creates, 0)
  assert.equal(f.calls.control.length, 0)
})

test('read-only recovery honors cancel and fresh-enumeration deadline', async () => {
  const reason = Object.assign(new Error('cancelled'), { name: 'AbortError' })
  const cancelled = fixture(); cancelled.controller.abort(reason)
  await assert.rejects(cancelled.recover(previousRom()), value => value === reason)
  assert.equal(cancelled.calls.creates, 0)
  const late = fixture({ snapshots: [[native()]], enumerationDelay: 250 })
  await assert.rejects(late.recover(previousRom()), { code: 'ROM_PORT_REBIND_TIMEOUT' })
  assert.equal(late.calls.creates, 0)
})


test('read-only recovery observes a real 2.4-second missing-device window before one handoff', async () => {
  const runtime = native('/dev/ttyACM2', { productId: 0x4023 })
  const f = fixture({ snapshots: [...Array.from({ length: 48 }, () => []), [runtime], [runtime], [rom('/dev/ttyACM3')]] })
  const result = await f.recover(previousRom(), { timeout: 5000 })
  assert.equal(f.now(), 2400)
  assert.equal(result.port, '/dev/ttyACM3')
  assert.equal(result.transition.recovery.runtimePid, 0x4023)
  assert.equal(f.calls.control.length, 1)
  assert.equal(f.calls.connect.length, 1)
  assert.equal(f.calls.disconnect, 1)
  assert.equal(f.calls.enumerate[48].timeout, 2600)
})

test('a stale ROM followed by disappearance is observed passively until the same runtime returns', async () => {
  const runtime = native('/dev/ttyACM2')
  const snapshots = [...Array.from({ length: 10 }, () => [rom('/dev/ttyACM0')]),
    ...Array.from({ length: 38 }, () => []), [runtime], [runtime], [rom('/dev/ttyACM3')]]
  const f = fixture({ snapshots })
  const result = await f.recover(previousRom(), { timeout: 5000 })
  assert.equal(f.now(), 2400)
  assert.equal(result.port, '/dev/ttyACM3')
  assert.equal(f.calls.creates, 1)
  assert.equal(f.calls.control.length, 1)
  assert.equal(f.calls.disconnect, 1)
})

test('a conflicting MAC or location seen after waiting fails before opening any runtime', async () => {
  for (const conflicting of [native('/dev/ttyACM2', { serialNumber: 'e8f60ae75653' }),
    native('/dev/ttyACM2', { location: '2-3' }), rom('/dev/ttyACM2', { serialNumber: 'e8f60ae75653' }),
    rom('/dev/ttyACM2', { location: '2-3' })]) {
    const f = fixture({ snapshots: [[], [], [conflicting]] })
    await assert.rejects(f.recover(previousRom()), { code: 'ROM_RUNTIME_IDENTITY_MISMATCH' })
    assert.equal(f.now(), 100)
    assert.equal(f.calls.creates, 0)
    assert.equal(f.calls.control.length, 0)
  }
})

test('late duplicate runtime or simultaneous ROM/runtime descriptors remain ambiguous', async () => {
  for (const second of [native('/dev/ttyACM3'), rom('/dev/ttyACM3')]) {
    const f = fixture({ snapshots: [[], [native('/dev/ttyACM2'), second]] })
    await assert.rejects(f.recover(previousRom()), { code: 'ROM_PORT_AMBIGUOUS' })
    assert.equal(f.now(), 50)
    assert.equal(f.calls.creates, 0)
    assert.equal(f.calls.control.length, 0)
  }
})

test('cancel during passive recovery sleep never creates a session or sends bootloader', async () => {
  const reason = Object.assign(new Error('cancelled during native USB observation'), { name: 'AbortError' })
  const f = fixture({ snapshots: [[]], onSleep(controller, clock) { if (clock >= 150) controller.abort(reason) } })
  await assert.rejects(f.recover(previousRom(), { timeout: 5000 }), value => value === reason)
  assert.equal(f.now(), 150)
  assert.equal(f.calls.creates, 0)
  assert.equal(f.calls.control.length, 0)
})

test('late runtime enumeration does not start handoff after the passive deadline', async () => {
  const runtime = native('/dev/ttyACM2')
  const f = fixture({ snapshots: [[runtime]], enumerationDelay: 201 })
  await assert.rejects(f.recover(previousRom()), { code: 'ROM_PORT_REBIND_TIMEOUT' })
  assert.equal(f.calls.creates, 0)
  assert.equal(f.calls.control.length, 0)
})

test('a runtime handshake error after settling is propagated without another observation or handoff', async () => {
  const runtime = native('/dev/ttyACM2')
  const reason = failure('SESSION_BUSY')
  const f = fixture({ snapshots: [[], [runtime], [runtime]], connectError: reason })
  await assert.rejects(f.recover(previousRom()), value => value === reason)
  assert.equal(f.now(), 50)
  assert.equal(f.calls.connect.length, 1)
  assert.equal(f.calls.creates, 1)
  assert.equal(f.calls.control.length, 0)
  assert.equal(f.calls.disconnect, 1)
})

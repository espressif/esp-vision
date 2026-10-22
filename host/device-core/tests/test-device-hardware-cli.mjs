// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { createServer } from 'node:net'
import { once } from 'node:events'
import { checkHardware, parseArgs, serialTransport } from './hardware-check.mjs'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { EV_MUX_REQUIRED_FEATURES, EV_MUX_REQUIRED_CHANNELS, EV_MUX_DEFAULT_LIMITS } from '../src/device_session.js'

test('hardware CLI validates transport, timeout and never accepts credential URLs', () => {
  assert.equal(parseArgs(['--tcp', 'localhost']).port, 3232)
  assert.equal(parseArgs(['--serial', '/dev/test', '--role', 'usb']).role, 'usb')
  assert.equal(parseArgs(['--tcp', '[::1]:1234', '--write']).host, '::1')
  for (const args of [[], ['--tcp', 'a', '--serial', 'b'], ['--tcp', 'tcp://user:secret@host'], ['--tcp', 'a:65536'], ['--serial', 'a', '--timeout', 'NaN'], ['--tcp', 'a', '--role', 'usb']]) assert.throws(() => parseArgs(args))
})

test('default hardware acceptance exercises real facade over a fake EV-MUX TCP peer', async () => {
  const sessionId = 'a'.repeat(32), methods = [], sockets = new Set(), messages = []
  const server = createServer(socket => {
    sockets.add(socket)
    socket.on('close', () => sockets.delete(socket))
    let pythonMarker = null
    const parser = new EvMuxParser({ filterFrame(metadata, bytes) {
      const text = new TextDecoder().decode(bytes)
      if (metadata.channel === 'repl.stdin') {
        pythonMarker ??= /EV_HARDWARE_[a-z0-9]+/.exec(text)?.[0] ?? null
        const completionMarker = /~~EVSCRIPT-END:[A-Za-z0-9-]+~~/.exec(text)?.[0]
        if (completionMarker) {
          socket.write(makeEvMuxFrame({ channel: 'repl.stdout', type: 'data', sessionId, vmEpoch: 1,
            payload: `${text}\r\n${pythonMarker ?? ''}\r\n${completionMarker}\r\n>>> ` }))
          return false
        }
        const marker = /EV_HARDWARE_[a-z0-9]+/.exec(text)?.[0] ?? 'EVIDE_BOOT_OK'
        socket.write(makeEvMuxFrame({ channel: 'repl.stdout', type: 'data', sessionId, vmEpoch: 1, payload: `${text}\r\n${marker}\r\n>>> ` }))
        return false
      }
      if (!metadata.channel.endsWith('.rpc')) return false
      methods.push(metadata.method)
      const request = JSON.parse(text)
      let result = { ok: true }
      if (metadata.method === 'hello') result = { ok: true, evMuxVersion: 4, deviceId: 'abcdef123456', hostname: 'esp-vision-test', board: 'TEST', firmware: { version: 'test' }, vmReady: true, session: { id: sessionId, vmEpoch: 1, leaseMs: 15000, keepaliveMs: 5000 } }
      if (metadata.method === 'capabilities') result = { ok: true, protocol: { evMuxVersion: 4 }, features: EV_MUX_REQUIRED_FEATURES, channels: EV_MUX_REQUIRED_CHANNELS, limits: { ...EV_MUX_DEFAULT_LIMITS, txLargePayloadMax: 262144 }, session: { policy: 'firstCome', leaseMs: 15000, keepaliveMs: 5000 } }
      socket.write(makeEvMuxFrame({ channel: metadata.channel, type: 'rsp', method: metadata.method, seq: metadata.seq, ...((metadata.method !== 'capabilities' && (metadata.method !== 'hello' || request.acquire)) ? { sessionId, vmEpoch: 1 } : {}), payload: JSON.stringify(result) }))
      return false
    } })
    socket.on('data', bytes => parser.feedBytes(bytes))
  })
  server.listen(0, '127.0.0.1')
  await once(server, 'listening')
  try {
    await checkHardware(parseArgs(['--tcp', `127.0.0.1:${server.address().port}`, '--timeout', '5000']), { nonce: 'abc123', report: value => messages.push(value) })
    assert.deepEqual(messages.map(value => value.check), ['inspect', 'python'])
    assert.ok(methods.includes('session.release'))
    assert.ok(!methods.includes('script.write'))
    assert.ok(!methods.includes('network.configure'))
  } finally { for (const socket of sockets) socket.destroy(); await new Promise(resolve => server.close(resolve)) }
})

function fakeDevice({ mismatch = false, collision = false, writeFails = false } = {}) {
  const calls = [], files = new Map(), path = '/evhc_abc123.bin'
  if (collision) files.set(path, new Uint8Array([7]))
  return {
    calls, files,
    async connect() { calls.push('connect') },
    async inspect() { return { identity: { board: 'TEST', firmwareVersion: 'test' }, storage: { flash: { present: true, mounted: true, writable: true } }, network: { password: 'NEVER_PRINT' } } },
    python: { async execute() { return { ok: true, output: 'EV_HARDWARE_abc123\n' } } },
    files: Object.assign(files, {
      async list() { return [...files.keys()].map(path => ({ path })) },
      async write({ path, data, overwrite }) { calls.push('write'); assert.equal(overwrite, false); files.set(`${path}.tmp`, data); if (writeFails) throw new Error('Write failed'); files.delete(`${path}.tmp`); files.set(path, data) },
      async read({ path }) { return mismatch ? new Uint8Array([0]) : files.get(path) },
      async remove({ path, signal }) { assert.equal(signal.aborted, false); calls.push('remove'); files.delete(path) },
    }),
    async disconnect() { calls.push('disconnect') },
  }
}

test('write acceptance validates bytes and deletes only its owned file, even after partial failure', async () => {
  for (const config of [{}, { mismatch: true }, { writeFails: true }]) {
    const device = fakeDevice(config), reports = []
    const run = checkHardware(parseArgs(['--tcp', 'localhost', '--write']), { device, nonce: 'abc123', report: value => reports.push(value) })
    if (config.mismatch || config.writeFails) await assert.rejects(run)
    else await run
    assert.equal(device.files.size, 0)
    assert.equal(device.calls.at(-1), 'disconnect')
    assert.ok(device.calls.includes('remove'))
    assert.ok(!JSON.stringify(reports).includes('NEVER_PRINT'))
  }
})

test('write acceptance refuses an existing test path and preserves it', async () => {
  const device = fakeDevice({ collision: true })
  await assert.rejects(checkHardware(parseArgs(['--tcp', 'localhost', '--write']), { device, nonce: 'abc123', report() {} }), { code: 'TEST_PATH_EXISTS' })
  assert.equal(device.files.size, 1)
  assert.deepEqual(device.calls, ['connect', 'disconnect'])
})

test('cancellation before connection has no file side effects and still disconnects', async () => {
  const device = fakeDevice(), controller = new AbortController()
  controller.abort()
  await assert.rejects(checkHardware(parseArgs(['--tcp', 'localhost']), { device, signal: controller.signal, report() {} }), { code: 'CANCELLED' })
  assert.equal(device.files.size, 0)
  assert.equal(device.calls.at(-1), 'disconnect')
})

test('missing optional Python executable fails cleanly without opening a port', async () => {
  const transport = serialTransport({ python: '/nonexistent-esp-vision-python', serial: '/not-a-device' })
  const unsubscribe = transport.subscribe({ error() {}, close() {} })
  try {
    await assert.rejects(transport.connect({ signal: AbortSignal.timeout(1000) }), { code: 'SERIAL_ERROR' })
  } finally { unsubscribe(); await transport.disconnect() }
})


test('expected board and firmware must match before any Python or file operations', async () => {
  for (const args of [['--expect-board', 'OTHER'], ['--expect-firmware-version', 'stale']]) {
    const device = fakeDevice()
    device.python.execute = async () => assert.fail('Wrong device must not execute Python')
    await assert.rejects(checkHardware(parseArgs(['--tcp', 'localhost', ...args]), { device, report() {} }), { code: args[0] === '--expect-board' ? 'BOARD_MISMATCH' : 'FIRMWARE_MISMATCH' })
    assert.deepEqual(device.calls, ['connect', 'disconnect'])
  }
})

test('explicit file acceptance reports unmounted storage and fails without writing or formatting', async () => {
  const device = fakeDevice(), inspect = device.inspect, reports = []
  device.inspect = async request => ({ ...await inspect(request), storage: { flash: { present: true, mounted: false, writable: true, reason: 'HOST_WRITER_ACTIVE' } } })
  await assert.rejects(checkHardware(parseArgs(['--tcp', 'localhost', '--write']), { device, nonce: 'abc123', report: value => reports.push(value) }), { code: 'STORAGE_UNAVAILABLE' })
  assert.deepEqual(device.calls, ['connect', 'disconnect'])
  assert.equal(reports.find(result => result.check === 'storage').flash.mounted, false)
  assert.equal(reports.find(result => result.check === 'storage').ok, false)
})

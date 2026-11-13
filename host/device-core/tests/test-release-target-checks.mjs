// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { runReleaseCheck } from './release-checks.mjs'

function peer() {
  const calls = [], variables = new Map()
  const identity = { deviceId: 'target', board: 'TEST', firmwareVersion: 'test' }
  return {
    calls, variables, identity,
    async inspect() { calls.push('inspect'); return { identity: { ...identity }, runtime: { vmReady: true } } },
    async disconnect() { calls.push('disconnect') },
    async connect() { calls.push('connect') },
    python: { async execute({ source }) {
      calls.push(source)
      let output
      if (source.startsWith('globals().pop')) {
        variables.clear()
        output = 'EV_DISCOVERY_CLEAN'
      } else if (source.startsWith('_evdiscovery')) {
        const [variable, value] = source.split(';')[0].split('=')
        output = JSON.parse(value)
        variables.set(variable, output)
      } else if (source.startsWith('print(_evdiscovery')) output = variables.get(source.slice(6, -1)) ?? 'NameError'
      else {
        const expression = source.slice(6, -1)
        const parts = expression.split('+')
        output = JSON.parse(parts[0])
        if (parts.length === 2) output += JSON.parse(parts[1].split('*')[0]).repeat(256)
      }
      return { ok: true, output: `${output}\n` }
    } },
  }
}
const options = { serial: '/dev/test', role: 'usj' }
const found = { sourcesAvailable: 1, warnings: [], devices: [{ address: '/dev/test', status: 'unprobed' }] }

test('passive discovery retains live VM state and cleans its sentinel on all outcomes', async () => {
  for (const mode of ['pass', 'missing', 'reset', 'unavailable']) {
    const device = peer()
    const operation = runReleaseCheck('discovery', device, options, { nonce: 'abc', discover: async request => {
      assert.equal(request.probe, false)
      assert.deepEqual(request.interfaces, ['serial'])
      if (mode === 'reset') device.variables.clear()
      if (mode === 'missing') return { ...found, devices: [] }
      if (mode === 'unavailable') return { ...found, warnings: [{ code: 'SERIAL_UNAVAILABLE' }] }
      return found
    } })
    if (mode === 'pass') assert.equal((await operation).vmStatePreserved, true)
    else await assert.rejects(operation, { code: { missing: 'DISCOVERY_TARGET_MISSING', reset: 'ROUNDTRIP_MISMATCH', unavailable: 'DISCOVERY_UNAVAILABLE' }[mode] })
    assert.equal(device.variables.size, 0)
    assert.ok(!device.calls.includes('disconnect'))
  }
})

test('communication requires current full Unicode output and interleaved identity', async () => {
  const device = peer()
  assert.equal((await runReleaseCheck('communication', device, options, { nonce: 'abc' })).rounds, 5)
  assert.equal(device.calls.filter(call => call === 'inspect').length, 6)
  for (const result of [{ ok: true, output: 'stale' }, { ok: false, output: '' }, { ok: true, outputTruncated: true, output: '' }]) {
    device.python.execute = async () => result
    await assert.rejects(runReleaseCheck('communication', device, options, { nonce: 'abc' }), { code: 'ROUNDTRIP_MISMATCH' })
  }
})

test('reconnect requires the same board and fresh round markers after each acquisition', async () => {
  const device = peer()
  assert.equal((await runReleaseCheck('reconnect', device, options, { nonce: 'abc' })).cycles, 3)
  assert.equal(device.calls.filter(call => call === 'connect').length, 3)
  device.connect = async () => { device.identity.deviceId = 'different-board' }
  await assert.rejects(runReleaseCheck('reconnect', device, options, { nonce: 'abc' }), { code: 'IDENTITY_CHANGED' })
})

test('TCP discovery must match both the runtime identity and selected endpoint', async () => {
  const device = peer(), tcp = { tcp: '192.0.2.1:3232', host: '192.0.2.1', port: 3232, role: 'tcp' }
  const row = { host: tcp.host, port: 3232, identity: device.identity, status: 'advertised' }
  const discover = async () => ({ ...found, devices: [row] })
  assert.equal((await runReleaseCheck('discovery', device, tcp, { nonce: 'abc', discover })).interface, 'tcp')
  row.host = '192.0.2.2'
  await assert.rejects(runReleaseCheck('discovery', device, tcp, { nonce: 'abc', discover }), { code: 'DISCOVERY_TARGET_MISSING' })
})

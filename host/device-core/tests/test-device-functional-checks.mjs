// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile, mkdtemp, rm } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { parseArgs } from './hardware-check.mjs'
import { DEVICE_CHECKS, parseCheckResult, runDeviceChecks, saveJpeg } from './device-checks.mjs'

function devicePeer({ results = {}, board = 'TEST', screenFails = false } = {}) {
  const files = new Map(), calls = [], listeners = new Map()
  return {
    calls, files, listeners,
    async inspect({ fields }) { calls.push(['inspect', fields]); return { identity: { board, firmwareVersion: 'test' }, capabilities: { protocol: { evMuxVersion: 4 }, features: [] }, runtime: { vmReady: true } } },
    files: Object.assign(files, {
      async list() { return [...files.keys()].map(path => ({ path })) },
      async write({ path, data, overwrite }) { calls.push(['write', path]); assert.equal(overwrite, false); files.set(path, data) },
      async remove({ path }) { calls.push(['remove', path]); files.delete(path) },
    }),
    python: { async execute({ source }) {
      calls.push(['execute', source])
      const name = /\['run_check'\]\("([a-z_]+)"/.exec(source)?.[1]
      if (!name) return { ok: true, output: '' }
      const result = results[name] ?? { name, status: 'PASS', data: { width: 240, height: 320 } }
      if (name === 'preview' && result.status === 'PASS' && !results.noPreview) listeners.get('preview')?.({ data: new Uint8Array([1]) })
      return { ok: true, output: `unrelated output\n__EVDEVICECHECK__${JSON.stringify(result)}\n` }
    } },
    on(event, listener) { listeners.set(event, listener); return () => listeners.delete(event) },
    async capture({ source }) {
      calls.push(['capture', source])
      if (source === 'screen' && screenFails) throw Object.assign(new Error('Not supported'), { code: 'UNSUPPORTED_OPERATION' })
      return { data: new Uint8Array([255, 216, 0, 255, 217]), mimeType: 'image/jpeg', metadata: { width: 240, height: 320 } }
    },
  }
}

test('functional CLI preserves all legacy checks, validates options and honors explicit screenshot request', () => {
  assert.deepEqual(parseArgs(['--tcp', 'test', '--checks', 'all']).checks, DEVICE_CHECKS)
  assert.equal(parseArgs(['--tcp', 'test', '--checks', 'all']).timeout, 600000)
  assert.equal(parseArgs(['--tcp', 'test', '--checks', 'all', '--timeout', '5000']).timeout, 5000)
  assert.deepEqual(parseArgs(['--serial', 'test', '--screen-output', 'screen.jpg']).checks, ['display'])
  for (const args of [['--checks', 'bogus'], ['--frames', '0'], ['--framesize', 'QVGA;print(1)'], ['--quality', '101'], ['--frame-timeout', '0'], ['--display-hold', '-1']]) assert.throws(() => parseArgs(['--tcp', 'test', ...args]))
})

test('device check reports reject stale, duplicated and malformed markers', () => {
  const valid = '__EVDEVICECHECK__{"name":"memory","status":"PASS","data":{"heapFree":1}}'
  assert.equal(parseCheckResult(valid, 'memory').data.heapFree, 1)
  for (const output of ['', valid + '\n' + valid, '__EVDEVICECHECK__bad', valid.replace('memory', 'camera'), valid.replace('PASS', 'UNKNOWN')]) assert.throws(() => parseCheckResult(output, 'memory'), { code: 'CHECK_REPORT_INVALID' })
})

test('functional checks stage their shared fixture once and consume preview via public events', async () => {
  const device = devicePeer(), reports = []
  await runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'memory,preview,orientation']), { nonce: 'abc123', report: value => reports.push(value) })
  assert.equal(device.calls.filter(call => call[0] === 'write').length, 1)
  assert.equal(device.files.size, 0)
  assert.equal(device.listeners.size, 0)
  assert.equal(reports.find(result => result.check === 'preview').data.received, 1)
  assert.deepEqual(reports.at(-1), { check: 'summary', passed: 3, failed: 0, skipped: 0 })
})

test('required IPA never silently skips; a failed check does not suppress later checks', async () => {
  const device = devicePeer({ board: 'ESP32_P4X_EYE', results: { ipa_stats: { name: 'ipa_stats', status: 'SKIP', error: { message: 'ESP_ERR_NOT_SUPPORTED' } } } }), reports = []
  await assert.rejects(runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'ipa_stats,identity']), { nonce: 'abc123', report: value => reports.push(value) }), { code: 'DEVICE_CHECKS_FAILED' })
  assert.equal(reports.find(result => result.check === 'ipa_stats').status, 'FAIL')
  assert.equal(reports.find(result => result.check === 'identity').status, 'PASS')
  assert.equal(device.files.size, 0)
})

test('missing LCD skips ordinary display tests but explicitly requested screenshot must fail', async () => {
  const response = { name: 'display', status: 'SKIP', error: { message: 'ESP_ERR_NOT_SUPPORTED' } }
  for (const explicit of [false, true]) {
    const device = devicePeer({ results: { display: response } }), reports = []
    const options = parseArgs(['--tcp', 'test', '--checks', 'display', ...(explicit ? ['--screen-output', 'screen.jpg'] : [])])
    const run = runDeviceChecks(device, options, { nonce: 'abc123', report: value => reports.push(value) })
    if (explicit) await assert.rejects(run, { code: 'DEVICE_CHECKS_FAILED' })
    else await run
    assert.equal(reports.find(result => result.check === 'display').status, explicit ? 'FAIL' : 'SKIP')
    assert.ok(device.calls.some(call => call[0] === 'execute' && call[1].includes("['close_display']()")))
  }
})

test('screen JPEG is captured while display fixture is alive and saved on host', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'esp-vision-screen-'))
  const output = join(directory, 'screen.jpg'), device = devicePeer()
  try {
    await runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'display', '--screen-output', output]), { nonce: 'abc123', report() {} })
    assert.deepEqual(await readFile(output), Buffer.from([255, 216, 0, 255, 217]))
    const captureAt = device.calls.findIndex(call => call[0] === 'capture' && call[1] === 'screen')
    const closeAt = device.calls.findIndex(call => call[0] === 'execute' && call[1].includes("['close_display']()"))
    assert.ok(captureAt > 0 && closeAt > captureAt)
    assert.equal(device.files.size, 0)
  } finally { await rm(directory, { recursive: true, force: true }) }
})

test('truncated JPEG and missing dimensions are refused before host artifact is written', async () => {
  await assert.rejects(saveJpeg({ data: new Uint8Array([255, 216]), mimeType: 'image/jpeg', metadata: { width: 1, height: 1 } }), { code: 'CAPTURE_INVALID' })
  await assert.rejects(saveJpeg({ data: new Uint8Array([255, 216, 255, 217]), mimeType: 'image/jpeg' }), { code: 'CAPTURE_INVALID' })
})

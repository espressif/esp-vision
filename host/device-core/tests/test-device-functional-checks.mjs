// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile, mkdtemp, rm, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { EV_REPL_INPUT_MAX_BYTES, evReplExecInputBytes } from '../src/evrepl.js'
import { tmpdir } from 'node:os'
import { existsSync } from 'node:fs'
import { createHash } from 'node:crypto'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { parseArgs } from './hardware-check.mjs'
import { DEFAULT_ESPDL_MODEL, DEFAULT_ESPDL_SHA256, DEVICE_CHECKS, parseCheckResult, runDeviceChecks, saveJpeg } from './device-checks.mjs'
import { jpegFixture } from './jpeg-fixtures.mjs'

function devicePeer({ results = {}, board = 'TEST', screenFails = false } = {}) {
  const files = new Map(), calls = [], listeners = new Map(), fixtureChunks = new Map()
  let modelNonce, modelBytes, modelPath, modelOffset = 0
  return {
    calls, files, listeners, fixtureChunks,
    async inspect({ fields }) { calls.push(['inspect', fields]); return { identity: { board, firmwareVersion: 'test' }, capabilities: { protocol: { evMuxVersion: 4 }, features: ['storage.writer.lease'], limits: { replInputMax: EV_REPL_INPUT_MAX_BYTES } }, runtime: { vmReady: true }, storage: { flash: { present: true, mounted: false, writable: false } } } },
    files: Object.assign(files, {
      async list() { throw new Error('Runtime checks must not access storage') },
      async write() { throw Object.assign(new Error('Flash unmounted'), { code: 'FILE_OPEN_FAILED' }) },
      async remove() { throw new Error('Runtime checks must not access storage') },
    }),
    python: { async execute({ source }) {
      calls.push(['execute', source])
      if (source.includes("={'_source':{},'_fixture_nonce':")) modelNonce = JSON.parse(/'_fixture_nonce':("[^"]+")/.exec(source)[1])
      const scoped = /^exec\((.*),_evhil_[a-z0-9]+\)$/.exec(source)
      if (scoped) {
        const body = JSON.parse(scoped[1])
        const phase = /\[_fixture_nonce,'([^']+)',/.exec(body)?.[1]
        if (phase) {
          let data
          if (phase === 'init') data = 0
          else if (phase === 'chunk') {
            const match = /_source.setdefault\((\d+),(.+)\)\]\]\)\)$/.exec(body)
            const offset = Number(match[1]), chunk = JSON.parse(match[2])
            if (!fixtureChunks.has(offset)) fixtureChunks.set(offset, chunk)
            data = [offset, fixtureChunks.get(offset)]
          } else {
            const text = [...fixtureChunks.values()].join(''), digest = createHash('sha256').update(text).digest('hex')
            data = phase === 'assemble' ? Buffer.byteLength(text) : phase === 'verify' ? digest : [digest, true, 0, false]
          }
          return { ok: true, output: '__EVHILFIXTURE__' + JSON.stringify([modelNonce, phase, data]) }
        }
        return { ok: true, output: '' }
      }
      const model = /\['model_command'\]\((.*)\)$/.exec(source)
      if (model) {
        const [operation, ...args] = JSON.parse('[' + model[1] + ']')
        let data
        if (operation === 'begin') {
          ;[modelNonce, modelBytes, modelPath] = args
          modelOffset = 0
          data = { size: modelBytes, offset: 0, path: modelPath }
        } else if (operation === 'append') {
          const [offset, encoded] = args, bytes = Buffer.from(encoded, 'base64').length
          modelOffset = offset + bytes
          data = { offset, end: modelOffset, bytes, current: modelOffset }
        } else if (operation === 'mount') data = { path: modelPath, bytes: modelBytes, sha256: args[0] }
        else data = { bytes: 0, offset: 0, mounted: false }
        return { ok: true, output: '__EVHILACK__' + JSON.stringify([modelNonce, operation, data]) }
      }
      if (source.startsWith('assert ') && source.includes('__EVHILACK__')) {
        return { ok: true, output: JSON.parse(source.slice(source.indexOf(';print(') + 7, -1)) }
      }
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
      return { data: jpegFixture('screen').data, mimeType: 'image/jpeg', metadata: { width: 240, height: 320 } }
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

test('functional checks load fixtures in RAM despite unmounted storage and consume public preview events', async () => {
  const device = devicePeer(), reports = []
  await runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'memory,preview,orientation']), { nonce: 'abc123', report: value => reports.push(value) })
  const commands = device.calls.filter(call => call[0] === 'execute').map(call => call[1])
  assert.ok(commands.every(source => evReplExecInputBytes(source) <= EV_REPL_INPUT_MAX_BYTES))
  assert.equal(commands.filter(source => source.includes('exec(_text,globals())')).length, 1)
  const uploaded = [...device.fixtureChunks.values()].join('')
  assert.equal(uploaded, await readFile(new URL('./fixtures/device-checks.py', import.meta.url), 'utf8'))
  assert.equal(device.files.size, 0)
  assert.equal(device.listeners.size, 0)
  assert.equal(reports.find(result => result.check === 'preview').data.received, 1)
  assert.deepEqual(reports.at(-1), { check: 'summary', passed: 3, failed: 0, skipped: 0, blocked: 0 })
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
    assert.deepEqual(await readFile(output), Buffer.from(jpegFixture('screen').data))
    const captureAt = device.calls.findIndex(call => call[0] === 'capture' && call[1] === 'screen')
    const closeAt = device.calls.findIndex(call => call[0] === 'execute' && call[1].includes("['close_display']()"))
    assert.ok(captureAt > 0 && closeAt > captureAt)
    assert.equal(device.files.size, 0)
  } finally { await rm(directory, { recursive: true, force: true }) }
})

test('camera acceptance explicitly publishes an application snapshot before passive observation', async () => {
  const device = devicePeer()
  await runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'camera']), { nonce: 'abc123', report() {} })
  const published = device.calls.findIndex(call => call[0] === 'execute' && call[1].includes("['run_check'](\"camera\""))
  const observed = device.calls.findIndex(call => call[0] === 'capture' && call[1] === 'camera')
  assert.ok(published > 0 && observed > published)
  assert.equal(device.files.size, 0)
})

test('truncated JPEG and missing dimensions are refused before host artifact is written', async () => {
  await assert.rejects(saveJpeg({ data: new Uint8Array([255, 216]), mimeType: 'image/jpeg', metadata: { width: 1, height: 1 } }), { code: 'CAPTURE_INVALID' })
  await assert.rejects(saveJpeg({ data: new Uint8Array([255, 216, 255, 217]), mimeType: 'image/jpeg' }), { code: 'CAPTURE_INVALID' })
})


test('strict acceptance fails unsupported selected checks without preventing independent identity evidence', async () => {
  const device = devicePeer({ results: { memory: { name: 'memory', status: 'SKIP', error: { message: 'ESP_ERR_NOT_SUPPORTED' } } } }), reports = []
  await assert.rejects(runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'memory,identity', '--require-all']), { nonce: 'abc123', report: value => reports.push(value) }), { code: 'DEVICE_CHECKS_FAILED' })
  assert.equal(reports.find(result => result.check === 'memory').status, 'FAIL')
  assert.equal(reports.find(result => result.check === 'identity').status, 'PASS')
  assert.equal(reports.find(result => result.check === 'identity').data.storage.flash.mounted, false)
})

test('failed partial RAM loading still releases its namespace without filesystem cleanup', async () => {
  const device = devicePeer(), reports = [], execute = device.python.execute
  let failed = false
  device.python.execute = async request => {
    if (!failed && request.source.includes('_source.setdefault(')) { failed = true; throw new Error('fixture load failed') }
    return execute(request)
  }
  await assert.rejects(runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'memory']), { nonce: 'abc123', report: value => reports.push(value) }), { code: 'DEVICE_CHECKS_FAILED' })
  assert.ok(device.calls.some(call => call[0] === 'execute' && call[1].includes('del _evhil_abc123')))
  assert.equal(reports.at(-1).failed, 1)
})

test('identity and RAM checks do not require optional storage inspection support', async () => {
  const device = devicePeer(), inspect = device.inspect, reports = []
  device.inspect = async request => {
    assert.ok(!request.fields.includes('storage'))
    const info = await inspect(request)
    info.capabilities.features = []
    return info
  }
  await runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'identity,memory']), { nonce: 'abc123', report: value => reports.push(value) })
  assert.deepEqual(reports.at(-1), { check: 'summary', passed: 2, failed: 0, skipped: 0, blocked: 0 })
})

test('P4 issue regressions are included in the strict target-check path', async () => {
  const required = ['camera_display_lifecycle', 'image_copy_to_fb', 'image_transform_argument', 'sensor_snapshot_buffer', 'memory_policy', 'isp_ccm_frames', 'isp_lsc_display']
  assert.ok(required.every(name => DEVICE_CHECKS.includes(name)))
  const device = devicePeer(), reports = []
  await runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', required.join(','), '--require-all']),
    { nonce: 'issue910', report: value => reports.push(value) })
  assert.deepEqual(reports.at(-1), { check: 'summary', passed: required.length, failed: 0, skipped: 0, blocked: 0 })
})

test('ESP-DL inference streams a model into RAM, respects the live REPL limit and never touches persistent storage', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'esp-vision-model-'))
  const model = join(directory, 'model.espdl'), bytes = Buffer.from(Array.from({ length: 1024 }, (_, index) => index & 255))
  try {
    await writeFile(model, bytes)
    const device = devicePeer(), reports = [], inspect = device.inspect
    device.inspect = async request => {
      assert.ok(!request.fields.includes('storage'))
      const info = await inspect(request)
      info.capabilities.limits.replInputMax = 256
      return info
    }
    await runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', model, '--require-all']), { nonce: 'abc123', report: value => reports.push(value) })
    const commands = device.calls.filter(call => call[0] === 'execute').map(call => call[1])
    assert.ok(commands.every(source => evReplExecInputBytes(source) <= 256))
    const uploaded = commands.filter(source => source.includes("['model_command'](\"append\",")).map(source => Buffer.from(JSON.parse('[' + source.slice(source.indexOf('](') + 2, -1) + ']')[2], 'base64'))
    assert.deepEqual(Buffer.concat(uploaded), bytes)
    assert.ok(commands.some(source => source.includes("['model_command'](\"mount\",\"" + createHash('sha256').update(bytes).digest('hex'))))
    assert.ok(commands.some(source => source.includes('modelPath\\\":\\\"/evhil_abc123/model.espdl')))
    assert.ok(commands.some(source => source.includes("['model_command'](\"close\")")))
    assert.equal(reports.find(result => result.check === 'espdl_inference').data.modelBytes, bytes.length)
    assert.equal(reports.find(result => result.check === 'espdl_inference').data.modelStorage, 'ram-vfs')
    assert.equal(device.files.size, 0)
  } finally { await rm(directory, { recursive: true, force: true }) }
})

test('missing ESP-DL model skips only optional acceptance and malformed expected hashes are rejected', async () => {
  assert.throws(() => parseArgs(['--tcp', 'test', '--model-sha256', 'bad']), { code: 'ARGUMENT' })
  for (const strict of [false, true]) {
    const device = devicePeer(), results = []
    const run = runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', join(tmpdir(), 'missing-esp-vision.espdl'), ...(strict ? ['--require-all'] : [])]), { nonce: 'abc123', report: value => results.push(value) })
    if (strict) await assert.rejects(run, { code: 'DEVICE_CHECKS_FAILED' })
    else await run
    assert.equal(results.find(result => result.check === 'espdl_inference').status, strict ? 'FAIL' : 'SKIP')
    assert.ok(!device.calls.some(call => call[0] === 'execute'))
  }
})

test('ESP-DL model hash mismatch is rejected before device mutation', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'esp-vision-model-'))
  try {
    const model = join(directory, 'model.espdl'), device = devicePeer(), reports = []
    await writeFile(model, Buffer.from([1, 2, 3]))
    await assert.rejects(runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', model, '--model-sha256', '0'.repeat(64)]), { nonce: 'abc123', report: value => reports.push(value) }), { code: 'DEVICE_CHECKS_FAILED' })
    assert.equal(reports.find(result => result.check === 'espdl_inference').error.code, 'MODEL_HASH_MISMATCH')
    assert.equal(device.calls.length, 0)
  } finally { await rm(directory, { recursive: true, force: true }) }
})

test('partial model upload, inference failure and cleanup failure cannot report success', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'esp-vision-model-'))
  try {
    const model = join(directory, 'model.espdl')
    await writeFile(model, Buffer.alloc(1024))
    for (const stage of ['append_model', 'espdl_inference', 'close_model']) {
      const device = devicePeer(), execute = device.python.execute, reports = []
      device.python.execute = async request => {
        if (request.source.includes(`['model_command']("${stage.replace('_model', '')}"`) || (stage === 'espdl_inference' && request.source.includes("['run_check'](\"espdl_inference\""))) {
          device.calls.push(['failed-execute', request.source])
          throw new Error('injected failure')
        }
        return execute(request)
      }
      await assert.rejects(runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', model, '--require-all']), { nonce: 'abc123', report: value => reports.push(value) }), { code: 'DEVICE_CHECKS_FAILED' })
      assert.equal(reports.find(result => result.check === 'espdl_inference').status, 'FAIL')
      assert.ok(device.calls.some(call => call[1]?.includes("['model_command'](\"close\")")))
      assert.ok(device.calls.some(call => call[1]?.includes(".get('close_model',lambda:None)()")))
    }
  } finally { await rm(directory, { recursive: true, force: true }) }
})

test('model transfer cancellation releases RAM resources with an independent cleanup signal', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'esp-vision-model-'))
  try {
    const model = join(directory, 'model.espdl'), device = devicePeer(), execute = device.python.execute, controller = new AbortController()
    await writeFile(model, Buffer.alloc(1024))
    let cleaned = false
    device.python.execute = async request => {
      if (request.source.includes("['model_command'](\"append\",")) controller.abort(new Error('cancelled transfer'))
      if (request.source.includes("['model_command'](\"close\")")) { assert.equal(request.signal.aborted, false); cleaned = true }
      return execute(request)
    }
    await assert.rejects(runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', model]), { signal: controller.signal, nonce: 'abc123', report() {} }), /cancelled transfer/)
    assert.equal(cleaned, true)
  } finally { await rm(directory, { recursive: true, force: true }) }
})

// Installed npm packages run these tests without the repository's models directory.
async function modelCase(callback, size = 1024) {
  const directory = await mkdtemp(join(tmpdir(), 'esp-vision-model-ack-'))
  try {
    const model = join(directory, 'model.espdl'), bytes = typeof size === 'number' ? Buffer.alloc(size, 42) : Buffer.from(size)
    await writeFile(model, bytes)
    await callback({ model, bytes, device: devicePeer(), reports: [] })
  } finally { await rm(directory, { recursive: true, force: true }) }
}

test('model upload retries a complete public execute only for uncertain TIMEOUT and reports recovery', async () => {
  for (const executed of [false, true]) await modelCase(async ({ model, device, reports }) => {
    const execute = device.python.execute, attempts = []
    let first = true
    device.python.execute = async request => {
      if (request.source.includes('[\'model_command\']("append",')) {
        attempts.push(request.source)
        if (first) {
          first = false
          if (executed) await execute(request)
          throw Object.assign(new Error('uncertain acknowledgement'), { code: 'TIMEOUT' })
        }
      }
      return execute(request)
    }
    await runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', model, '--require-all']), { nonce: 'acktest', report: value => reports.push(value) })
    assert.equal(attempts[0], attempts[1])
    const data = reports.find(result => result.check === 'espdl_inference').data
    assert.equal(data.modelRetries, 1)
    assert.deepEqual(data.modelRetryOffsets, [{ offset: 0, retries: 1 }])
    assert.equal(attempts.length, data.modelChunks + 1)
  })
})

test('model upload limits timeout attempts to three and never retries cancellation or semantic errors', async () => {
  for (const mode of ['timeout', 'error', 'abort']) await modelCase(async ({ model, device, reports }) => {
    const execute = device.python.execute, controller = new AbortController()
    let attempts = 0
    device.python.execute = async request => {
      if (request.source.includes('[\'model_command\']("append",')) {
        attempts++
        const error = Object.assign(new Error(mode), { code: mode === 'error' ? 'DATA_CONFLICT' : 'TIMEOUT' })
        if (mode === 'abort') controller.abort(error)
        throw error
      }
      return execute(request)
    }
    await assert.rejects(runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', model, '--require-all']), { signal: controller.signal, nonce: 'acktest', report: value => reports.push(value) }))
    assert.equal(attempts, mode === 'timeout' ? 3 : 1)
  })
})

test('every model operation requires a semantic ACK even when Python prints an exception with ok true', async () => {
  for (const operation of ['begin', 'append', 'mount', 'close', 'cleanup']) await modelCase(async ({ model, device, reports }) => {
    const execute = device.python.execute
    let attempts = 0
    device.python.execute = async request => {
      if (request.source.includes(`['model_command']("${operation}"`) || (operation === 'cleanup' && request.source.startsWith('assert '))) {
        attempts++
        return { ok: true, output: 'Traceback (most recent call last):\nOSError: injected failure\n' }
      }
      return execute(request)
    }
    await assert.rejects(runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', model, '--require-all']), { nonce: 'acktest', report: value => reports.push(value) }), { code: 'DEVICE_CHECKS_FAILED' })
    assert.equal(attempts, 1)
    assert.ok(reports.some(result => result.status === 'FAIL'))
  })
})

test('upload ACK rejects stale nonce, wrong offset, duplicate markers and malformed output without retry', async () => {
  for (const invalid of ['nonce', 'offset', 'duplicate', 'malformed']) await modelCase(async ({ model, device, reports }) => {
    const execute = device.python.execute
    let attempts = 0
    device.python.execute = async request => {
      const response = await execute(request)
      if (request.source.includes('[\'model_command\']("append",')) {
        attempts++
        const ack = JSON.parse(response.output.slice('__EVHILACK__'.length))
        if (invalid === 'nonce') ack[0] = 'stale'
        if (invalid === 'offset') ack[2].current++
        response.output = '__EVHILACK__' + JSON.stringify(ack)
        if (invalid === 'duplicate') response.output += '\n' + response.output
        if (invalid === 'malformed') response.output = '__EVHILACK__broken'
      }
      return response
    }
    await assert.rejects(runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', model]), { nonce: 'acktest', report: value => reports.push(value) }), { code: 'DEVICE_CHECKS_FAILED' })
    assert.equal(attempts, 1)
    assert.equal(reports.find(result => result.check === 'espdl_inference').error.code, 'MODEL_ACK_INVALID')
  })
})

test('begin timeout before its body preserves the original error and still acknowledges empty cleanup', async () => {
  await modelCase(async ({ model, device, reports }) => {
    const execute = device.python.execute
    device.python.execute = async request => {
      if (request.source.includes('[\'model_command\']("begin",')) throw Object.assign(new Error('begin preparation timeout'), { code: 'TIMEOUT' })
      return execute(request)
    }
    await assert.rejects(runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', model]), { nonce: 'acktest', report: value => reports.push(value) }), { code: 'DEVICE_CHECKS_FAILED' })
    assert.equal(reports.find(result => result.check === 'espdl_inference').error.code, 'TIMEOUT')
    assert.equal(reports.at(-1).failed, 1)
  })
})

test('full-sized model upload reserves the largest offset and final block within both live REPL budgets', async () => {
  const modelBytes = existsSync(DEFAULT_ESPDL_MODEL) ? await readFile(DEFAULT_ESPDL_MODEL) : Buffer.alloc(495840, 42)
  for (const limit of [256, 511]) await modelCase(async ({ model, device, reports, bytes }) => {
    const inspect = device.inspect
    device.inspect = async request => { const result = await inspect(request); result.capabilities.limits.replInputMax = limit; return result }
    try { await runDeviceChecks(device, parseArgs(['--tcp', 'test', '--checks', 'espdl_inference', '--model', model]), { nonce: '0123456789abcdef', report: value => reports.push(value) }) }
    catch { assert.fail(JSON.stringify({ limit, reports })) }
    const commands = device.calls.filter(call => call[0] === 'execute').map(call => call[1])
    assert.ok(commands.every(source => evReplExecInputBytes(source) <= limit))
    const chunks = commands.filter(source => source.includes('[\'model_command\']("append",')).map(source => JSON.parse('[' + source.slice(source.indexOf('](') + 2, -1) + ']'))
    let offset = 0
    for (const [, actualOffset, encoded] of chunks) { assert.equal(actualOffset, offset); offset += Buffer.from(encoded, 'base64').length }
    assert.equal(offset, bytes.length)
    assert.deepEqual(Buffer.concat(chunks.map(chunk => Buffer.from(chunk[2], 'base64'))), bytes)
    assert.equal(reports.find(result => result.check === 'espdl_inference').data.modelChunks, limit === 511 ? 1517 : 3673)
  }, modelBytes)
})

test('repository checkouts provide the default ESP-DL model', { skip: !existsSync(new URL('../../../models/', import.meta.url)) }, async () => {
  const bytes = await readFile(DEFAULT_ESPDL_MODEL)
  assert.equal(bytes.length, 495840)
  assert.equal(createHash('sha256').update(bytes).digest('hex'), DEFAULT_ESPDL_SHA256)
})

test('ISP cleanup releases camera and collects memory even when display deinit fails', t => {
  const python = [process.env.PYTHON, 'python3', 'python'].filter(Boolean).find(command => spawnSync(command, ['-c', 'pass'], { windowsHide: true }).status === 0)
  if (!python) return t.skip('Python is required to execute the device-side cleanup fixture')
  const source = `
import runpy, sys, types
fixture = runpy.run_path(sys.argv[1])
for display_fails, camera_fails in ((False, False), (True, False), (False, True), (True, True)):
    events = []
    class Display:
        def deinit(self):
            events.append('display')
            if display_fails:
                raise RuntimeError('display cleanup failed')
    def shutdown():
        events.append('camera')
        if camera_fails:
            raise RuntimeError('camera cleanup failed')
    sys.modules['sensor'] = types.SimpleNamespace(shutdown=shutdown)
    sys.modules['gc'] = types.SimpleNamespace(collect=lambda: events.append('collect'))
    try:
        fixture['isp_release_camera_lcd'](Display())
    except RuntimeError:
        assert display_fails or camera_fails
    else:
        assert not display_fails and not camera_fails
    assert events == ['display', 'camera', 'collect'], events
`
  const result = spawnSync(python, ['-c', source, fileURLToPath(new URL('./fixtures/device-checks.py', import.meta.url))], { encoding: 'utf8', windowsHide: true })
  assert.equal(result.status, 0, result.stderr)
})

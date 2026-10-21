// SPDX-License-Identifier: Apache-2.0
// Application-level HIL checks. All device I/O goes through createDevice's API.
import { readFile, mkdir, writeFile } from 'node:fs/promises'
import { dirname } from 'node:path'
import { EV_REPL_INPUT_MAX_BYTES, evReplExecInputBytes } from '../src/evrepl.js'

export const DEVICE_CHECKS = Object.freeze(['identity', 'memory', 'camera', 'framesizes', 'pixformats', 'orientation', 'ipa_stats', 'snapshot_rate', 'preview', 'image_ops', 'display', 'display_stream', 'camera_display_lifecycle', 'image_copy_to_fb', 'image_transform_argument', 'sensor_snapshot_buffer', 'memory_policy'])
const MARKER = '__EVDEVICECHECK__'
const failure = (code, message) => Object.assign(new Error(message), { code })
const unsupported = error => error?.code === 'UNSUPPORTED_OPERATION' || /ESP_ERR_NOT_SUPPORTED|ESP_ERR_NOT_FOUND|no module named|ENODEV/i.test(error?.message ?? '')
function cancelled(signal) { if (signal?.aborted) throw signal.reason instanceof Error ? signal.reason : failure('CANCELLED', 'Device checks cancelled') }

export function parseCheckResult(output, name) {
  const lines = output.split(/\r?\n/).filter(line => line.startsWith(MARKER))
  if (lines.length !== 1) throw failure('CHECK_REPORT_INVALID', 'Expected exactly one device check report')
  let result
  try { result = JSON.parse(lines[0].slice(MARKER.length)) } catch { throw failure('CHECK_REPORT_INVALID', 'Invalid device check JSON') }
  if (!result || result.name !== name || !['PASS', 'SKIP', 'FAIL'].includes(result.status)) throw failure('CHECK_REPORT_INVALID', 'Unexpected device check result')
  if (result.status === 'PASS' && (!result.data || typeof result.data !== 'object' || Array.isArray(result.data))) throw failure('CHECK_REPORT_INVALID', 'Missing device measurements')
  if (result.status !== 'PASS' && (!result.error || typeof result.error.message !== 'string')) throw failure('CHECK_REPORT_INVALID', 'Missing device failure details')
  return result
}

export async function saveJpeg(image, output) {
  const data = image?.data
  if (image?.mimeType !== 'image/jpeg' || !(data instanceof Uint8Array) || data.length < 4 || data[0] !== 255 || data[1] !== 216 || data.at(-2) !== 255 || data.at(-1) !== 217) throw failure('CAPTURE_INVALID', 'Capture is not a complete JPEG')
  if (!(image.metadata?.width > 0 && image.metadata?.height > 0)) throw failure('CAPTURE_INVALID', 'Capture has no valid image dimensions')
  if (output) { await mkdir(dirname(output), { recursive: true }); await writeFile(output, data) }
  return { bytes: data.length, width: image.metadata.width, height: image.metadata.height }
}

function pause(milliseconds, signal) {
  return new Promise((resolve, reject) => {
    const cleanup = () => { clearTimeout(timer); signal?.removeEventListener('abort', abort) }
    const abort = () => { cleanup(); reject(signal.reason ?? failure('CANCELLED', 'Device check cancelled')) }
    const timer = setTimeout(() => { cleanup(); resolve() }, milliseconds)
    signal?.addEventListener('abort', abort, { once: true })
    if (signal?.aborted) abort()
  })
}

export async function runDeviceChecks(device, options, { signal, nonce, report = value => console.log(JSON.stringify(value)) } = {}) {
  const results = [], namespace = `_evhil_${nonce}`
  let loaded = false, namespaceStarted = false, loadError = null, inputLimit = EV_REPL_INPUT_MAX_BYTES
  const parameters = { frames: options.frames, framesize: options.framesize, pixformat: options.pixformat, displayHold: options.displayHold, screenCapture: Boolean(options.screenOutput) }
  const timeoutFor = name => 1000 * (name === 'display' ? 30 + options.displayHold * 3 : name === 'camera_display_lifecycle' ? options.frameTimeout + 80 : name === 'memory_policy' ? options.frameTimeout + 60 : name === 'sensor_snapshot_buffer' ? options.frameTimeout + 40 : name === 'image_copy_to_fb' ? 30 : name === 'ipa_stats' ? options.frameTimeout + 40 : options.frameTimeout + Math.max(30, options.frames * (name === 'display_stream' ? 2 : 1)))
  const execute = async (source, timeout = 10000, operationSignal = signal) => {
    // Larger python.execute() requests use temporary files in Device Core.
    // Keep fixtures in RAM by respecting the actual encoded REPL-line budget.
    if (evReplExecInputBytes(source) > inputLimit) throw failure('REPL_INPUT_LIMIT', 'Hardware fixture command exceeds the device REPL input limit')
    const result = await device.python.execute({ source, timeout, maxOutputChars: 16384, signal: operationSignal })
    if (result.ok === false || result.outputTruncated) throw failure('PYTHON_CHECK', 'Device Python check did not complete')
    return result.output
  }
  const load = async () => {
    if (loaded) return
    if (loadError) throw loadError
    try {
      const info = await device.inspect({ fields: ['capabilities'], signal })
      const advertised = info.capabilities?.limits?.replInputMax
      if (Number.isSafeInteger(advertised) && advertised > 0) inputLimit = Math.min(inputLimit, advertised)
      const source = await readFile(new URL('./fixtures/device-checks.py', import.meta.url), 'utf8')
      namespaceStarted = true
      await execute(`${namespace}={'_source':[]}`)
      const append = chunk => `${namespace}['_source'].append(${JSON.stringify(chunk)})`
      let chunk = ''
      for (const character of source) {
        if (evReplExecInputBytes(append(chunk + character)) > inputLimit) {
          if (!chunk) throw failure('REPL_INPUT_LIMIT', 'Device REPL input limit cannot load hardware fixtures')
          await execute(append(chunk))
          chunk = ''
        }
        chunk += character
      }
      if (chunk) await execute(append(chunk))
      await execute(`exec(''.join(${namespace}['_source']),${namespace});del ${namespace}['_source']`)
      loaded = true
    } catch (error) { loadError = error; throw error }
  }
  const invoke = async name => {
    await load()
    const output = await execute(`${namespace}['run_check'](${JSON.stringify(name)},${JSON.stringify(JSON.stringify(parameters))})`, timeoutFor(name))
    return parseCheckResult(output, name)
  }
  const requirePass = result => {
    if (result.status !== 'PASS') throw Object.assign(failure(result.status === 'SKIP' ? 'UNSUPPORTED_OPERATION' : 'DEVICE_CHECK_FAILED', result.error.message), { checkResult: result })
    return result.data
  }
  const record = result => { results.push(result); report({ check: result.name, ...result, name: undefined }) }
  try {
    for (const name of options.checks) {
      cancelled(signal)
      const started = Date.now()
      let result
      try {
        let data
        if (name === 'identity') {
          const info = await device.inspect({ fields: ['identity', 'capabilities', 'runtime'], signal })
          if (info.capabilities?.features?.includes('storage.writer.lease')) {
            info.storage = (await device.inspect({ fields: ['storage'], signal })).storage
          }
          if (!info.identity?.board || !info.identity.firmwareVersion || info.capabilities?.protocol?.evMuxVersion !== 4 || info.runtime?.vmReady !== true) throw failure('IDENTITY_INVALID', 'Expected a ready EV-MUX v4 firmware identity')
          data = { identity: info.identity, features: info.capabilities.features, vmReady: info.runtime.vmReady,
            storage: info.storage && Object.fromEntries(Object.entries(info.storage).map(([volume, state]) => [volume, { present: state.present, mounted: state.mounted, writable: state.writable, reason: state.reason }])) }
        } else if (name === 'camera') {
          const image = await device.capture({ source: 'camera', format: 'jpeg', quality: options.quality, timeout: options.frameTimeout * 1000, signal })
          data = await saveJpeg(image, options.output)
          data.sensor = requirePass(await invoke('sensor_state'))
        } else if (name === 'preview') {
          let count = 0
          const unsubscribe = device.on('preview', image => { if (image.data?.length) count++ })
          try {
            data = requirePass(await invoke(name))
            await pause(2000, signal)
            if (!count) throw failure('PREVIEW_EMPTY', 'No preview frame arrived')
            data.received = count
          } finally { unsubscribe() }
        } else {
          const response = await invoke(name)
          if (name === 'ipa_stats' && response.status === 'SKIP') {
            const info = await device.inspect({ fields: ['identity'], signal })
            const board = (info.identity?.board ?? '').toUpperCase().replaceAll('-', '_')
            if (['ESP32_P4X_EYE', 'ESP32_P4X_FUNCTION_EV_BOARD'].includes(board)) throw failure('IPA_REQUIRED', 'IPA statistics are required on this board')
          }
          data = requirePass(response)
          if (name === 'display' && options.screenOutput) {
            const image = await device.capture({ source: 'screen', format: 'jpeg', quality: options.quality, timeout: options.frameTimeout * 1000, signal })
            const captured = await saveJpeg(image, options.screenOutput)
            if (captured.width !== data.width || captured.height !== data.height) throw failure('SCREEN_SIZE_MISMATCH', 'LCD capture dimensions differ from display dimensions')
            data.screen = captured
          }
        }
        result = { name, status: 'PASS', data }
      } catch (error) {
        cancelled(signal)
        const skip = unsupported(error) && !options.requireAll && !(name === 'display' && options.screenOutput)
        result = { name, status: skip ? 'SKIP' : 'FAIL', error: { code: error.code ?? 'DEVICE_CHECK_FAILED', message: String(error.message ?? 'Device check failed').slice(0, 240) } }
      } finally {
        if (name === 'display' && loaded) {
          try { await execute(`${namespace}['close_display']()`, 5000, AbortSignal.timeout(6000)) }
          catch { result = { name, status: 'FAIL', error: { code: 'DISPLAY_CLEANUP_FAILED', message: 'Could not release the test display' } } }
        }
      }
      result.seconds = (Date.now() - started) / 1000
      record(result)
    }
  } finally {
    const cleanupSignal = AbortSignal.timeout(10000)
    let cleanupError
    if (namespaceStarted) {
      try { await execute(`if ${JSON.stringify(namespace)} in globals():\n ${namespace}.get('close_display',lambda:None)()\n del ${namespace}`, 5000, cleanupSignal) }
      catch (error) { cleanupError = error }
    }
    if (cleanupError) record({ name: 'fixture-cleanup', status: 'FAIL', error: { code: 'FIXTURE_CLEANUP_FAILED', message: 'Could not release the hardware fixture namespace' } })
  }
  const summary = { passed: results.filter(result => result.status === 'PASS').length, failed: results.filter(result => result.status === 'FAIL').length, skipped: results.filter(result => result.status === 'SKIP').length }
  report({ check: 'summary', ...summary })
  if (summary.failed) throw Object.assign(failure('DEVICE_CHECKS_FAILED', 'One or more device checks failed'), { results })
  return results
}

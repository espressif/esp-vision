// SPDX-License-Identifier: Apache-2.0
// Application-level HIL checks. All device I/O goes through createDevice's API.
import { readFile, mkdir, writeFile } from 'node:fs/promises'
import { dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createHash } from 'node:crypto'
import { EV_REPL_INPUT_MAX_BYTES, evReplExecInputBytes } from '../src/evrepl.js'
import { RELEASE_CHECKS, runReleaseCheck } from './release-checks.mjs'
import { validateJpegImage } from '../src/jpeg_validate.js'
import { loadFixture } from './fixture-loader.mjs'

export const DEVICE_CHECKS = Object.freeze([...RELEASE_CHECKS, 'modules', 'ulab', 'identity', 'memory', 'camera', 'framesizes', 'pixformats', 'orientation', 'ipa_stats', 'snapshot_rate', 'preview', 'image_ops', 'display', 'display_stream', 'camera_display_lifecycle', 'image_copy_to_fb', 'image_transform_argument', 'sensor_snapshot_buffer', 'memory_policy', 'espdl_inference', 'isp_ccm_frames', 'isp_lsc_display'])
// The npm package ships tests without models; repository checkouts use the bundled face detector.
export const DEFAULT_ESPDL_SHA256 = '04eec2220adbf642a843c953817fb219a06c094aedb76c181ba2a958c65201dc'
export const DEFAULT_ESPDL_MODEL = fileURLToPath(new URL('../../../models/espdet/pico/face/espdet_pico_224_224_face.espdl', import.meta.url))
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
  if (image?.mimeType !== 'image/jpeg') throw failure('CAPTURE_INVALID', 'Capture is not a JPEG')
  try { validateJpegImage(data, image.metadata) } catch { throw failure('CAPTURE_INVALID', 'Capture is not a structurally valid JPEG with matching dimensions') }
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
  const timeoutFor = name => 1000 * (name === 'display' ? 30 + options.displayHold * 3 : name === 'camera_display_lifecycle' ? options.frameTimeout + 80 : name === 'memory_policy' ? options.frameTimeout + 60 : name === 'sensor_snapshot_buffer' ? options.frameTimeout + 40 : name === 'image_copy_to_fb' ? 30 : name === 'espdl_inference' ? options.frameTimeout + 60 + options.frames * 2 : name === 'ipa_stats' ? options.frameTimeout + 40 : options.frameTimeout + Math.max(30, options.frames * (name === 'display_stream' ? 2 : 1)))
  const execute = async (source, timeout = 10000, operationSignal = signal) => {
    // Larger python.execute() requests use temporary files in Device Core.
    // Keep fixtures in RAM by respecting the actual encoded REPL-line budget.
    if (evReplExecInputBytes(source) > inputLimit) throw failure('REPL_INPUT_LIMIT', 'Hardware fixture command exceeds the device REPL input limit')
    const result = await device.python.execute({ source, timeout, maxOutputChars: 16384, signal: operationSignal })
    if (result.ok === false || result.outputTruncated) throw failure('PYTHON_CHECK', 'Device Python check did not complete')
    return result.output
  }
  const acknowledged = (output, operation, expected) => {
    let reply
    try {
      const line = output.trim()
      if (!line.startsWith('__EVHILACK__')) throw new Error()
      reply = JSON.parse(line.slice('__EVHILACK__'.length))
    } catch { throw failure('MODEL_ACK_INVALID', 'Missing or malformed hardware fixture acknowledgement') }
    // Python dictionaries need not preserve the host's insertion order.
    if (!Array.isArray(reply) || reply.length !== 3 || reply[0] !== nonce || reply[1] !== operation || !reply[2] || typeof reply[2] !== 'object' || Array.isArray(reply[2]) ||
        Object.keys(reply[2]).length !== Object.keys(expected).length || Object.entries(expected).some(([key, value]) => reply[2][key] !== value)) {
      throw failure('MODEL_ACK_INVALID', 'Hardware fixture acknowledgement does not match the operation')
    }
  }
  const modelSource = (operation, args) => `${namespace}['model_command'](${JSON.stringify(operation)}${args.length ? ',' + args.map(value => JSON.stringify(value)).join(',') : ''})`
  const modelCall = async (operation, args, expected, { retry = false, timeout = 10000, operationSignal = signal } = {}) => {
    for (let attempt = 1; ; attempt++) {
      cancelled(operationSignal)
      try {
        // Re-enter the public API for a fresh source variable and its existing
        // Ctrl-C + exec('1') synchronization. Never replay an SDK += line.
        acknowledged(await execute(modelSource(operation, args), timeout, operationSignal), operation, expected)
        return attempt - 1
      } catch (error) {
        cancelled(operationSignal)
        if (!retry || error.code !== 'TIMEOUT' || attempt === 3) throw error
      }
    }
  }
  const load = async () => {
    if (loaded) return
    if (loadError) throw loadError
    let phase = 'inspect'
    const started = Date.now()
    try {
      const info = await device.inspect({ fields: ['capabilities'], signal })
      const advertised = info.capabilities?.limits?.replInputMax
      if (Number.isSafeInteger(advertised) && advertised > 0) inputLimit = Math.min(inputLimit, advertised)
      phase = 'read'
      const source = await readFile(new URL('./fixtures/device-checks.py', import.meta.url), 'utf8')
      namespaceStarted = true
      await loadFixture(source, { namespace, nonce, inputLimit, execute, signal, report })
      loaded = true
    } catch (error) {
      const diagnostics = error.bootstrap ?? { phase, elapsedMs: Date.now() - started, lastSuccessfulOperation: null }
      if (signal?.aborted) {
        report({ check: 'fixture-bootstrap', event: 'cancelled', diagnostics })
        cancelled(signal)
      }
      loadError = Object.assign(failure('FIXTURE_UNAVAILABLE', 'Blocked by fixture-bootstrap; the device check did not run'), { diagnostics })
      record({ name: 'fixture-bootstrap', status: 'FAIL', error: { code: error.code ?? 'FIXTURE_BOOTSTRAP_FAILED', message: 'Could not initialize the shared Python fixture' }, diagnostics, seconds: (Date.now() - started) / 1000 })
      throw loadError
    }
  }
  const invoke = async (name, extra = {}) => {
    await load()
    const selected = name === 'espdl_inference' ? { frames: parameters.frames, framesize: parameters.framesize, ...extra } : { ...parameters, ...extra }
    const output = await execute(`${namespace}['run_check'](${JSON.stringify(name)},${JSON.stringify(JSON.stringify(selected))})`, timeoutFor(name))
    return parseCheckResult(output, name)
  }
  const requirePass = result => {
    if (result.status !== 'PASS') throw Object.assign(failure(result.status === 'SKIP' ? 'UNSUPPORTED_OPERATION' : 'DEVICE_CHECK_FAILED', result.error.message), { checkResult: result })
    return result.data
  }
  // ESPDet accepts VFS streams, including a read-only BytesIO-backed mount.
  // Keep inference acceptance independent of persistent storage provisioning.
  const espdlInference = async () => {
    let model
    try { model = new Uint8Array(await readFile(options.model ?? DEFAULT_ESPDL_MODEL)) }
    catch (error) { throw failure('MODEL_UNAVAILABLE', `Cannot read ESP-DL model: ${error.code ?? 'read failed'}`) }
    if (!model.length || model.length > 2 * 1024 * 1024) throw failure('MODEL_INVALID', 'ESP-DL acceptance model must be 1 byte to 2 MiB')
    const sha256 = createHash('sha256').update(model).digest('hex')
    const expected = options.modelSha256 ?? (options.model ? null : DEFAULT_ESPDL_SHA256)
    if (expected && expected !== sha256) throw failure('MODEL_HASH_MISMATCH', 'ESP-DL model differs from the expected SHA-256')
    await load()
    const path = `/evhil_${nonce}`
    let started = false, chunks = 0, retries = 0
    const retryOffsets = []
    const transferStarted = Date.now()
    try {
      started = true
      await modelCall('begin', [nonce, model.length, path], { size: model.length, offset: 0, path })
      const append = (chunk, offset) => modelSource('append', [offset, Buffer.from(chunk).toString('base64')])
      // Base64 carries binary model bytes without the temporary-file execution
      // path or a second host protocol implementation. Honor the live limit.
      const chunkBytes = Math.floor((inputLimit - evReplExecInputBytes(append([], model.length))) / 4) * 3
      if (chunkBytes < 3) throw failure('REPL_INPUT_LIMIT', 'Device REPL input limit cannot load the inference model')
      for (let offset = 0; offset < model.length; offset += chunkBytes) {
        cancelled(signal)
        const chunk = model.subarray(offset, offset + chunkBytes), end = offset + chunk.length
        const recovered = await modelCall('append', [offset, Buffer.from(chunk).toString('base64')], { offset, end, bytes: chunk.length, current: end }, { retry: true })
        retries += recovered
        if (recovered) retryOffsets.push({ offset, retries: recovered })
        chunks++
      }
      await modelCall('mount', [sha256], { path, bytes: model.length, sha256 })
      const modelTransferMs = Date.now() - transferStarted
      const data = requirePass(await invoke('espdl_inference', { modelPath: `${path}/model.espdl` }))
      return { ...data, modelBytes: model.length, modelSha256: sha256, modelStorage: 'ram-vfs', modelChunks: chunks, modelTransferMs, modelRetries: retries, modelRetryOffsets: retryOffsets }
    } finally {
      if (started) {
        try { await modelCall('close', [], { bytes: 0, offset: 0, mounted: false }, { timeout: 5000, operationSignal: AbortSignal.timeout(6000) }) }
        catch { throw failure('MODEL_CLEANUP_FAILED', 'Could not release the temporary RAM model mount') }
      }
    }
  }
  const record = result => { results.push(result); report({ check: result.name, ...result, name: undefined }) }
  try {
    for (const name of options.checks) {
      cancelled(signal)
      const started = Date.now()
      let result
      try {
        // All checks except identity depend on this shared prerequisite. Do not
        // start model reads, preview listeners or captures after it has failed.
        if (name !== 'identity' && loadError) throw loadError
        let data
        if (RELEASE_CHECKS.includes(name)) {
          data = await runReleaseCheck(name, device, options, { signal, nonce })
        } else if (name === 'identity') {
          const info = await device.inspect({ fields: ['identity', 'capabilities', 'runtime'], signal })
          if (info.capabilities?.features?.includes('storage.writer.lease')) {
            info.storage = (await device.inspect({ fields: ['storage'], signal })).storage
          }
          if (!info.identity?.board || !info.identity.firmwareVersion || info.capabilities?.protocol?.evMuxVersion !== 4 || info.runtime?.vmReady !== true) throw failure('IDENTITY_INVALID', 'Expected a ready EV-MUX v4 firmware identity')
          data = { identity: info.identity, features: info.capabilities.features, vmReady: info.runtime.vmReady,
            storage: info.storage && Object.fromEntries(Object.entries(info.storage).map(([volume, state]) => [volume, { present: state.present, mounted: state.mounted, writable: state.writable, reason: state.reason }])) }
        } else if (name === 'camera') {
          // --capture explicitly opts into application camera setup. The public
          // capture API itself only observes a previously published safe frame.
          requirePass(await invoke('camera'))
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
        } else if (name === 'espdl_inference') {
          data = await espdlInference()
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
        const skip = (unsupported(error) || error.code === 'MODEL_UNAVAILABLE') && !options.requireAll && !(name === 'display' && options.screenOutput)
        result = { name, status: error === loadError ? 'BLOCKED' : skip ? 'SKIP' : 'FAIL', error: { code: error.code ?? 'DEVICE_CHECK_FAILED', message: String(error.message ?? 'Device check failed').slice(0, 240) } }
        if (error === loadError) result.blockedBy = 'fixture-bootstrap'
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
      try {
        await execute(`if ${JSON.stringify(namespace)} in globals():\n ${namespace}.get('close_display',lambda:None)()\n ${namespace}.get('close_model',lambda:None)()\n del ${namespace}`, 5000, cleanupSignal)
        const marker = '__EVHILACK__' + JSON.stringify([nonce, 'cleanup', { absent: true }])
        acknowledged(await execute(`assert ${JSON.stringify(namespace)} not in globals();print(${JSON.stringify(marker)})`, 5000, cleanupSignal), 'cleanup', { absent: true })
      }
      catch (error) { cleanupError = error }
    }
    if (cleanupError) record({ name: 'fixture-cleanup', status: 'FAIL', error: { code: 'FIXTURE_CLEANUP_FAILED', message: 'Could not release the hardware fixture namespace' } })
  }
  const summary = { passed: results.filter(result => result.status === 'PASS').length, failed: results.filter(result => result.status === 'FAIL').length, skipped: results.filter(result => result.status === 'SKIP').length, blocked: results.filter(result => result.status === 'BLOCKED').length }
  report({ check: 'summary', ...summary })
  if (summary.failed || summary.blocked) throw Object.assign(failure('DEVICE_CHECKS_FAILED', 'One or more device checks failed or were blocked'), { results })
  return results
}

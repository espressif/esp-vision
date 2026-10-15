// SPDX-License-Identifier: Apache-2.0
import { randomBytes } from 'node:crypto'
import { fileURLToPath } from 'node:url'
import { resolve } from 'node:path'
import { tcpTransport, serialTransport } from '../src/node_transport.js'
export { tcpTransport, serialTransport } from '../src/node_transport.js'
import { createDevice } from '../src/index.js'
import { DEVICE_CHECKS, runDeviceChecks, saveJpeg } from './device-checks.mjs'

export const usage = 'Usage: npm run test:hardware -- (--tcp HOST[:PORT] | --serial PATH) [--role usj|usb|uart] [--python PYTHON] [--write] [--capture] [--checks all|CSV] [--frames N] [--framesize QVGA] [--pixformat RGB565] [--quality 60] [--frame-timeout SECONDS] [--display-hold SECONDS] [--output CAMERA.jpg] [--screen-output LCD.jpg] [--timeout MILLISECONDS]'
const fail = (code, message) => Object.assign(new Error(message), { code })

export function parseArgs(args) {
  const options = { timeout: 60000, write: false, capture: false, python: 'python3', frames: 30, framesize: 'QVGA', pixformat: 'RGB565', quality: 60, frameTimeout: 25, displayHold: 0 }
  let explicitTimeout = false
  for (let index = 0; index < args.length; index++) {
    const option = args[index]
    if (option === '--help') return { help: true }
    if (option === '--write' || option === '--capture') { options[option.slice(2)] = true; continue }
    if (!['--tcp', '--serial', '--role', '--python', '--timeout', '--checks', '--screen-output', '--output', '--frames', '--display-hold', '--framesize', '--pixformat', '--quality', '--frame-timeout'].includes(option)) throw fail('ARGUMENT', 'Unknown argument')
    const value = args[++index]
    if (!value || value.startsWith('--')) throw fail('ARGUMENT', 'Missing argument value')
    const key = option.slice(2).replace(/-([a-z])/g, (_match, letter) => letter.toUpperCase())
    options[key] = value
    if (key === 'timeout') explicitTimeout = true
  }
  if (Boolean(options.tcp) === Boolean(options.serial)) throw fail('ARGUMENT', 'Select exactly one transport')
  if (options.checks) {
    options.checks = options.checks === 'all' ? [...DEVICE_CHECKS] : [...new Set(options.checks.split(',').map(name => name.trim()))]
    if (options.checks.some(name => !DEVICE_CHECKS.includes(name))) throw fail('ARGUMENT', 'Unknown device check')
  } else options.checks = []
  if (options.screenOutput && !options.checks.includes('display')) options.checks.push('display')
  if (options.checks.length && !explicitTimeout) options.timeout = 600000
  for (const key of ['frames', 'quality', 'frameTimeout', 'displayHold']) options[key] = Number(options[key])
  if (!Number.isInteger(options.frames) || options.frames < 1 || options.frames > 1000) throw fail('ARGUMENT', 'Frames must be 1 to 1000')
  if (!Number.isInteger(options.quality) || options.quality < 1 || options.quality > 100) throw fail('ARGUMENT', 'Quality must be 1 to 100')
  if (!Number.isFinite(options.frameTimeout) || options.frameTimeout <= 0 || options.frameTimeout > 120) throw fail('ARGUMENT', 'Frame timeout must be positive and at most 120 seconds')
  if (!Number.isFinite(options.displayHold) || options.displayHold < 0 || options.displayHold > 60) throw fail('ARGUMENT', 'Display hold must be 0 to 60 seconds')
  if (![options.framesize, options.pixformat].every(value => /^[A-Z][A-Z0-9_]{0,30}$/.test(value))) throw fail('ARGUMENT', 'Sensor settings must be uppercase constant names')
  if (options.output && !options.checks.includes('camera')) options.capture = true
  options.timeout = Number(options.timeout)
  if (!Number.isSafeInteger(options.timeout) || options.timeout < 1000 || options.timeout > 600000) throw fail('ARGUMENT', 'Timeout must be 1000 to 600000 milliseconds')
  if (options.tcp) {
    const match = /^(?:\[([0-9a-fA-F:]+)\]|([^\s:/]+))(?::([0-9]+))?$/.exec(options.tcp)
    if (!match || /[@?#]/.test(options.tcp)) throw fail('ARGUMENT', 'Expected TCP host[:port] without credentials or URL')
    options.host = match[1] ?? match[2]
    options.port = Number(match[3] ?? 3232)
    if (options.port < 1 || options.port > 65535) throw fail('ARGUMENT', 'Invalid TCP port')
    if (options.role) throw fail('ARGUMENT', '--role only applies to serial')
    options.role = 'tcp'
  } else {
    options.role ??= 'usj'
    if (!['usj', 'usb', 'uart'].includes(options.role)) throw fail('ARGUMENT', 'Serial role must be usj, usb or uart')
  }
  return options
}

async function bounded(operation, milliseconds) {
  let timer
  try { return await Promise.race([operation(), new Promise((_resolve, reject) => { timer = setTimeout(() => reject(fail('CLEANUP_TIMEOUT', 'Cleanup timed out')), milliseconds) })]) }
  finally { clearTimeout(timer) }
}

/** Uses only the public facade; injectable for deterministic non-hardware checks. */
export async function checkHardware(options, { device = createDevice({ transport: options.tcp ? tcpTransport(options) : serialTransport(options) }), signal, report = value => console.log(JSON.stringify(value)), nonce = randomBytes(8).toString('hex') } = {}) {
  const controller = new AbortController()
  const abort = () => controller.abort(fail('CANCELLED', 'Hardware check cancelled'))
  signal?.addEventListener('abort', abort, { once: true })
  if (signal?.aborted) abort()
  const timer = setTimeout(() => controller.abort(fail('TIMEOUT', 'Hardware check timed out')), options.timeout)
  let error, ownedPath = null, writeStarted = false
  const path = `/evhc_${nonce}.bin`
  const cancelled = () => { if (controller.signal.aborted) throw controller.signal.reason }
  try {
    await device.connect({ role: options.role, timeout: Math.min(options.timeout, 30000), signal: controller.signal })
    cancelled()
    const status = await device.inspect({ fields: ['identity', 'capabilities', 'runtime'], signal: controller.signal })
    // Only selected public identity fields are logged; never network credentials or Python output.
    report({ check: 'inspect', ok: true, identity: { deviceId: status.identity?.deviceId ?? null, board: status.identity?.board ?? null, hostname: status.identity?.hostname ?? null } })
    const marker = `EV_HARDWARE_${nonce}`
    const execution = await device.python.execute({ source: `print('${marker}')`, timeout: 10000, maxOutputChars: 1024, signal: controller.signal })
    if (!execution.ok || !execution.output.split(/\r?\n/).some(line => line.trim() === marker)) throw fail('PYTHON_CHECK', 'Python marker was not received')
    report({ check: 'python', ok: true })
    if (options.write) {
      const entries = await device.files.list({ path: '/', signal: controller.signal })
      if (entries.some(entry => entry.path === path || entry.path === `${path}.tmp`)) throw fail('TEST_PATH_EXISTS', 'Test path already exists; refusing to modify it')
      ownedPath = path
      const bytes = Uint8Array.from({ length: 1024 }, (_value, index) => (index * 73 + 29) & 255)
      writeStarted = true
      await device.files.write({ path, data: bytes, overwrite: false, signal: controller.signal })
      const read = await device.files.read({ path, maxBytes: bytes.length, timeout: 15000, signal: controller.signal })
      if (read.length !== bytes.length || !bytes.every((byte, index) => byte === read[index])) throw fail('READBACK_MISMATCH', 'File readback mismatch')
      report({ check: 'write-read', ok: true, bytes: bytes.length })
    }
    if (options.capture && !options.checks.includes('camera')) {
      const image = await device.capture({ source: 'camera', format: 'jpeg', quality: options.quality, timeout: options.frameTimeout * 1000, signal: controller.signal })
      const captured = await saveJpeg(image, options.output)
      report({ check: 'capture', ok: true, ...captured })
    }
    if (options.checks.length) await runDeviceChecks(device, options, { signal: controller.signal, nonce, report })
    cancelled()
  } catch (caught) { error = caught }
  finally {
    clearTimeout(timer)
    signal?.removeEventListener('abort', abort)
    if (ownedPath && writeStarted) {
      try {
        await bounded(async () => {
          const cleanupSignal = AbortSignal.timeout(5000)
          const entries = await device.files.list({ path: '/', signal: cleanupSignal })
          for (const entry of entries) if ([ownedPath, `${ownedPath}.tmp`].includes(entry.path)) await device.files.remove({ path: entry.path, signal: cleanupSignal })
        }, 6000)
        report({ check: 'file-cleanup', ok: true })
      } catch (caught) { report({ check: 'file-cleanup', ok: false, path: ownedPath }); error ??= caught }
    }
    try { await bounded(() => device.disconnect(), 10000) }
    catch (caught) { error ??= caught }
  }
  if (error) throw error
  return { ok: true }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const controller = new AbortController()
  const cancel = () => controller.abort()
  process.once('SIGINT', cancel)
  process.once('SIGTERM', cancel)
  try {
    const options = parseArgs(process.argv.slice(2))
    if (options.help) console.log(usage)
    else await checkHardware(options, { signal: controller.signal })
  } catch (error) {
    const code = typeof error?.code === 'string' && /^[A-Z0-9_]+$/.test(error.code) ? error.code : 'HARDWARE_CHECK_FAILED'
    console.error(JSON.stringify({ ok: false, code }))
    process.exitCode = 1
  } finally {
    process.removeListener('SIGINT', cancel)
    process.removeListener('SIGTERM', cancel)
  }
}

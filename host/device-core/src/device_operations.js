/* SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */
import { withDeviceOperation } from './device_python.js'

function failure(code, message) { return Object.assign(new Error(message), { code }) }
function record(value, keys, label) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new TypeError(`${label} must be an object`)
  for (const key of Object.keys(value)) if (!keys.includes(key)) throw new TypeError(`Unknown ${label} field: ${key}`)
}
function options(value, fallback) {
  const { timeout = fallback, signal } = value
  if (!Number.isFinite(timeout) || timeout <= 0) throw new RangeError('timeout must be positive')
  if (signal !== undefined && (!signal || typeof signal.addEventListener !== 'function' || typeof signal.aborted !== 'boolean')) throw new TypeError('signal must be an AbortSignal')
  aborted(signal)
  return { timeout, signal }
}
function aborted(signal) {
  if (!signal?.aborted) return
  if (signal.reason instanceof Error) throw signal.reason
  throw Object.assign(new Error('Device operation cancelled'), { name: 'AbortError' })
}
function pause(ms, signal) {
  return new Promise((resolve, reject) => {
    const cleanup = () => { clearTimeout(timer); signal?.removeEventListener('abort', cancel) }
    const cancel = () => { cleanup(); try { aborted(signal) } catch (error) { reject(error) } }
    const timer = setTimeout(() => { cleanup(); resolve() }, ms)
    signal?.addEventListener('abort', cancel, { once: true })
    if (signal?.aborted) cancel()
  })
}
function supported(session, feature) {
  if (!session.hasFeature(feature)) throw failure('UNSUPPORTED_OPERATION', `Firmware does not support ${feature}`)
}

/** Group configuration, control and observation without exposing protocol details. */
export function createDeviceOperations({ session, state = {} }) {
  const api = {
    async inspect(request = {}) {
      record(request, ['fields', 'timeout', 'signal'], 'inspect')
      const opts = options(request, 5000)
      const fields = request.fields ?? ['identity', 'capabilities', 'runtime']
      if (!Array.isArray(fields) || !fields.length || fields.some(field => !['identity', 'capabilities', 'runtime', 'network', 'storage'].includes(field))) throw new TypeError('fields must name identity, capabilities, runtime, network or storage')
      const result = {}
      for (const field of new Set(fields)) {
        aborted(opts.signal)
        if (field === 'identity') result.identity = session.identity ? structuredClone(session.identity) : null
        if (field === 'capabilities') result.capabilities = structuredClone(session.capabilities)
        if (field === 'runtime') result.runtime = {
          ...structuredClone(session.snapshot()),
          vmReady: session.vmReady,
          vmEpoch: session.vmEpoch,
          run: state.run ? { path: state.run.path ?? null, completion: state.run.completion, status: state.run.status } : null,
        }
        if (field === 'network') { supported(session, 'network.background'); result.network = await session.networkStatus(opts) }
        if (field === 'storage') {
          supported(session, 'storage.writer.lease')
          result.storage = {}
          for (const volume of ['flash', 'sd']) result.storage[volume] = await session.rpc('device.control', { action: 'storage.writer.status', volume }, opts)
        }
      }
      return result
    },

    async configure(request) {
      record(request, ['network', 'timeout', 'signal'], 'configure')
      const opts = options(request, 45000)
      record(request.network, ['enabled', 'credentials'], 'network')
      // Snapshot the patch before awaiting RPCs; callers may reuse their UI state.
      const network = { ...request.network }
      if (network.credentials && typeof network.credentials === 'object') network.credentials = { ...network.credentials }
      const hasCredentials = Object.hasOwn(network, 'credentials')
      const hasEnabled = Object.hasOwn(network, 'enabled')
      if (!hasCredentials && !hasEnabled) throw new TypeError('network must specify enabled or credentials')
      if (hasEnabled && typeof network.enabled !== 'boolean') throw new TypeError('network.enabled must be a boolean')
      if (hasCredentials && network.credentials !== null) {
        record(network.credentials, ['ssid', 'password'], 'credentials')
        const { ssid, password } = network.credentials
        const encode = value => new TextEncoder().encode(value).length
        if (typeof ssid !== 'string' || encode(ssid) < 1 || encode(ssid) > 32 || ssid.includes('\0')) throw new TypeError('ssid must contain 1 to 32 UTF-8 bytes without NUL')
        if (typeof password !== 'string' || !(password === '' || /^[\x20-\x7e]{8,63}$/.test(password) || /^[0-9a-fA-F]{64}$/.test(password))) throw new TypeError('password must be empty, 8 to 63 printable ASCII characters, or a 64-digit hexadecimal PSK')
      }
      if (hasCredentials && network.credentials === null && network.enabled === true) throw new TypeError('Cannot enable networking while clearing credentials')
      supported(session, 'network.background')
      if (hasCredentials) supported(session, 'network.configure')
      return withDeviceOperation(state, 'configure', async () => {
        const deadline = Date.now() + opts.timeout
        const completed = []
        let pending = null
        const remaining = () => {
          aborted(opts.signal)
          const timeout = deadline - Date.now()
          if (timeout <= 0) throw failure('TIMEOUT', 'Network configuration deadline exceeded')
          return { timeout: Math.min(timeout, 4000), signal: opts.signal }
        }
        const apply = async (action, invoke) => {
          const response = await invoke(remaining())
          pending = { action, operationId: response.operationId ?? null }
          if (response.accepted !== true || typeof response.operationId !== 'string' || !response.operationId) throw failure('INVALID_RESPONSE', 'Network change requires accepted=true and an operationId')
          if (response.operationId) {
            while (true) {
              const status = await session.networkStatus(remaining())
              const operation = [status.operation, status.lastOperation].find(item => item?.id === response.operationId)
              if (operation?.state === 'failed') throw failure(operation.error?.code ?? 'CONFIGURATION_FAILED', operation.error?.message ?? `${action} failed`)
              if (operation?.state === 'succeeded') break
              await pause(Math.min(100, remaining().timeout), opts.signal)
            }
          }
          completed.push({ ...pending })
          pending = null
        }
        try {
          const before = await session.networkStatus(remaining())
          if (typeof before.enabled !== 'boolean') throw failure('INVALID_RESPONSE', 'Network status is missing enabled state')
          // Clearing credentials necessarily disconnects and disables networking.
          const desiredEnabled = network.credentials === null ? false : (hasEnabled ? network.enabled : before.enabled)
          let currentEnabled = before.enabled
          if (hasCredentials) {
            if (network.credentials === null) {
              await apply('network.forget', rpcOpts => session.forgetNetwork(rpcOpts))
              currentEnabled = false
            } else {
              await apply('network.configure', rpcOpts => session.configureNetwork(network.credentials, rpcOpts))
              currentEnabled = true
            }
          }
          if (currentEnabled !== desiredEnabled) await apply('network.enable', rpcOpts => session.setNetworkEnabled(desiredEnabled, rpcOpts))
          return { ok: true, completed, network: await session.networkStatus(remaining()) }
        } catch (error) {
          // Acknowledged changes are not rolled back or represented as atomic.
          error.completed = completed
          if (pending) error.pending = pending
          throw error
        }
      })
    },

    async control(request) {
      record(request, ['action', 'mode', 'timeout', 'signal'], 'control')
      const opts = options(request, 15000)
      if (!['stop', 'reset', 'bootloader'].includes(request.action)) throw new TypeError('action must be stop, reset or bootloader')
      if (request.action === 'reset' && !['soft', 'hard'].includes(request.mode)) throw new TypeError('reset requires mode soft or hard')
      if (request.action !== 'reset' && Object.hasOwn(request, 'mode')) throw new TypeError('mode is only valid for reset')
      if (state.operation && state.operation.kind !== 'python.execute') throw failure('DEVICE_BUSY', 'Another device operation is active')
      const invoke = async () => {
        if (request.action === 'stop') {
          if (session.hasFeature('device.control')) return session.rpc('device.control', { action: 'stop' }, opts)
          await session.writeFrame('repl.signal', 'signal', null, new Uint8Array([3]), opts)
          return { ok: true }
        }
        return session.control(request.action === 'reset' ? `${request.mode}-reset` : 'bootloader', opts)
      }
      // An interrupt joins its Python run; otherwise reserve the device until
      // the command completes so new writes cannot race a reset.
      if (state.operation?.kind === 'python.execute') return invoke()
      return withDeviceOperation(state, 'control', invoke)
    },

    async capture(request) {
      record(request, ['source', 'format', 'quality', 'timeout', 'signal'], 'capture')
      const opts = options(request, 15000)
      if (!['camera', 'screen'].includes(request.source)) throw new TypeError('source must be camera or screen')
      if (request.format !== undefined && request.format !== 'jpeg') throw new TypeError('Only jpeg capture is supported')
      if (request.quality !== undefined && (!Number.isInteger(request.quality) || request.quality < 1 || request.quality > 100)) throw new RangeError('quality must be an integer from 1 to 100')
      const method = request.source === 'camera' ? 'debug.capture_frame' : 'debug.capture_screen'
      supported(session, method)
      return withDeviceOperation(state, 'capture', () => session.captureImage(method, request.quality === undefined ? {} : { quality: request.quality }, opts))
    },
  }
  return Object.freeze(api)
}

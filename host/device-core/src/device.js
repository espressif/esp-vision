/* SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */
import { DeviceSession } from './device_session.js'
import { EvMuxParser, makeEvMuxFrame } from './evmux.js'
import { EvRepl } from './evrepl.js'
import { NetworkDiscovery } from './network_discovery.js'
import { createPythonApi } from './device_python.js'
import { createFilesApi } from './device_files.js'
import { createDeviceOperations } from './device_operations.js'
import { checkFirmware, updateFirmware } from './firmware_updates.js'

const EVENTS = new Set(['python.output', 'raw', 'preview', 'log', 'state', 'diagnostic', 'rpc'])
function object(value, keys, name) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new TypeError(`${name} must be an object`)
  for (const key of Object.keys(value)) if (!keys.includes(key)) throw new TypeError(`Unknown ${name} field: ${key}`)
}
function failure(code, message) { return Object.assign(new Error(message), { code }) }
function cancellation(signal) {
  return signal.reason instanceof Error ? signal.reason : Object.assign(new Error('Device operation cancelled'), { name: 'AbortError' })
}
function abortable(value, signal) {
  return new Promise((resolve, reject) => {
    const abort = () => { cleanup(); reject(cancellation(signal)) }
    const cleanup = () => signal.removeEventListener('abort', abort)
    signal.addEventListener('abort', abort, { once: true })
    Promise.resolve(value).then(result => { cleanup(); resolve(result) }, error => { cleanup(); reject(error) })
    if (signal.aborted) abort()
  })
}

/** Application API. Byte framing, REPL implementation and RPC names stay private. */
class Device {
  #transport
  #discovery
  #session = null
  #api = null
  #state = {}
  #listeners = new Map()
  #unsubscribe = null
  #lifecycle = null
  #connecting = false
  #disconnecting = null
  #generation = 0
  #updating = false

  constructor(options = {}) {
    object(options, ['transport', 'discovery'], 'device options')
    this.#transport = options.transport ?? null
    this.#discovery = options.discovery ? new NetworkDiscovery(options.discovery) : null
    this.firmware = Object.freeze({
      check: request => checkFirmware(request, this.#session?.identity),
      update: request => this.#updateFirmware(request),
    })
    this.python = Object.freeze({
      input: data => this.#ready().python.input(data),
      execute: request => this.#invoke('python', 'execute', request),
    })
    this.files = Object.freeze(Object.fromEntries(['list', 'read', 'write', 'remove', 'mkdir'].map(method => [
      method, request => this.#invoke('files', method, request),
    ])))
  }

  on(event, handler) {
    if (!EVENTS.has(event)) throw new TypeError(`Unknown device event: ${event}`)
    if (typeof handler !== 'function') throw new TypeError('Event handler must be a function')
    const handlers = this.#listeners.get(event) ?? new Set()
    handlers.add(handler)
    this.#listeners.set(event, handlers)
    return () => { handlers.delete(handler); if (!handlers.size) this.#listeners.delete(event) }
  }

  #emit(event, value) {
    for (const handler of [...(this.#listeners.get(event) ?? [])]) {
      try { handler(value) } catch {
        // UI listeners must not break framing, response delivery or cleanup.
        if (event !== 'diagnostic') this.#emit('diagnostic', { event: 'listener.error', source: event })
      }
    }
  }

  async #updateFirmware(request) {
    if (this.#updating || this.#connecting || this.#disconnecting || this.#state.operation) throw failure('DEVICE_BUSY', 'Another device operation is active')
    const identity = this.#session?.identity ? structuredClone(this.#session.identity) : null
    this.#updating = true
    try { return await updateFirmware(request, identity, () => this.disconnect()) }
    finally { this.#updating = false }
  }

  #ready() {
    if (this.#updating) throw failure('DEVICE_BUSY', 'Firmware update is active')
    if (!this.#session || this.#session.state !== 'ready' || !this.#api) throw failure('DEVICE_NOT_READY', 'Connect the device before invoking this operation')
    return this.#api
  }

  async #invoke(domain, method, request = {}) {
    const api = this.#ready()
    if (!request || typeof request !== 'object' || Array.isArray(request)) throw new TypeError('Device request must be an object')
    const controller = new AbortController()
    const lifecycle = this.#lifecycle.signal
    const source = request.signal
    if (source !== undefined && (!source || typeof source.addEventListener !== 'function' || typeof source.aborted !== 'boolean')) throw new TypeError('signal must be an AbortSignal')
    const abortLifecycle = () => controller.abort(lifecycle.reason)
    const abortSource = () => controller.abort(source.reason)
    lifecycle.addEventListener('abort', abortLifecycle, { once: true })
    source?.addEventListener('abort', abortSource, { once: true })
    if (lifecycle.aborted) abortLifecycle()
    if (source?.aborted) abortSource()
    const cleanup = () => {
      lifecycle.removeEventListener('abort', abortLifecycle)
      source?.removeEventListener('abort', abortSource)
    }
    try {
      const result = await api[domain][method]({ ...request, signal: controller.signal })
      // A started Python run retains cancellation until its completion.
      if (result?.done instanceof Promise) { result.done.then(cleanup, cleanup); return result }
      cleanup()
      return result
    } catch (error) { cleanup(); throw error }
  }

  discover(options = {}) {
    if (!this.#discovery) throw failure('DISCOVERY_UNAVAILABLE', 'Provide a discovery adapter to list LAN devices')
    return this.#discovery.list(options)
  }

  async connect(options = {}) {
    object(options, ['transport', 'role', 'expectedDevice', 'timeout', 'signal'], 'connect')
    const { role = 'unknown', expectedDevice = null, timeout = 30000, signal } = options
    if (!['unknown', 'usj', 'usb', 'uart', 'tcp'].includes(role)) throw new TypeError('role must be usj, usb, uart or tcp')
    if (!Number.isFinite(timeout) || timeout <= 0) throw new RangeError('timeout must be positive')
    if (signal !== undefined && (!signal || typeof signal.addEventListener !== 'function' || typeof signal.aborted !== 'boolean')) throw new TypeError('signal must be an AbortSignal')
    if (signal?.aborted) throw cancellation(signal)
    if (this.#updating || this.#connecting || this.#disconnecting || this.#session) throw failure('DEVICE_BUSY', 'Disconnect before opening another connection')
    const transport = options.transport ?? this.#transport
    if (typeof transport?.writeBytes !== 'function' || typeof transport?.subscribe !== 'function') throw new TypeError('Transport requires writeBytes(bytes) and subscribe({data,error,close})')
    this.#connecting = true
    this.#transport = transport
    this.#state = {}
    const generation = ++this.#generation
    const lifecycle = this.#lifecycle = new AbortController()
    const abort = () => lifecycle.abort(cancellation(signal))
    signal?.addEventListener('abort', abort, { once: true })
    const timer = setTimeout(() => lifecycle.abort(failure('TIMEOUT', 'Device connection timed out')), timeout)
    const session = this.#session = new DeviceSession({
      transport, EvMuxParser, EvRepl, makeEvMuxFrame,
      handlers: {
        text: text => this.#emit('python.output', text),
        rawText: text => this.#emit('raw', text),
        log: text => this.#emit('log', text),
        preview: (base64, metadata, bytes) => this.#emit('preview', { data: bytes.slice(), mimeType: 'image/jpeg', metadata }),
        rpc: event => { this.#emit('rpc', event); if (event.metadata?.method === 'hello' || event.metadata?.method === 'network.changed') this.#emit('state', { state: session.state, reason: event.metadata.method }) },
        rpcRequest: event => this.#emit('diagnostic', { event: 'rpc.request', ...event }),
        rpcResponse: event => this.#emit('diagnostic', { event: 'rpc.response', ...event }),
        rpcTimeout: event => this.#emit('diagnostic', { event: 'rpc.timeout', ...event }),
        diagnostic: event => this.#emit('diagnostic', event),
        sessionLost: event => lost(event?.error ?? failure('SESSION_LOST', 'Device session was lost')),
      },
    })
    const lost = error => {
      if (generation !== this.#generation) return
      const reason = error instanceof Error ? error : failure('CONNECTION_CLOSED', 'Device transport closed')
      lifecycle.abort(reason)
      session.close(reason)
      if (!this.#disconnecting) {
        this.#disconnecting = this.#dispose(reason).catch(() => {
          this.#emit('diagnostic', { event: 'disconnect.cleanup_failed' })
        }).finally(() => { this.#disconnecting = null })
      }
    }
    this.#emit('state', { state: 'connecting' })
    try {
      this.#unsubscribe = transport.subscribe({ data: bytes => { if (this.#session === session) session.feedBytes(bytes) }, error: lost, close: () => lost() })
      if (typeof this.#unsubscribe !== 'function') throw new TypeError('Transport subscribe must synchronously return an unsubscribe function')
      if (transport.connect) await abortable(transport.connect({ signal: lifecycle.signal }), lifecycle.signal)
      await abortable(session.negotiate({ role, expectedDevice, signal: lifecycle.signal }), lifecycle.signal)
      if (lifecycle.signal.aborted || this.#session !== session) throw cancellation(lifecycle.signal)
      this.#api = {
        python: createPythonApi({ session, state: this.#state }),
        files: createFilesApi({ session, state: this.#state }),
        operations: createDeviceOperations({ session, state: this.#state }),
      }
      this.#emit('state', { state: 'ready', identity: structuredClone(session.identity) })
      return this.#api.operations.inspect()
    } catch (error) {
      if (this.#disconnecting) await this.#disconnecting.catch(() => {})
      else if (this.#session === session) await this.#dispose(error).catch(() => {})
      throw error
    } finally {
      clearTimeout(timer)
      signal?.removeEventListener('abort', abort)
      this.#connecting = false
    }
  }

  async #dispose(reason = failure('CONNECTION_CLOSED', 'Device disconnected')) {
    const session = this.#session
    const transport = this.#transport
    ++this.#generation
    this.#lifecycle?.abort(reason)
    let error
    try { if (session?.sessionId) await session.release() } catch (caught) { error = caught }
    finally {
      session?.close(reason)
      this.#session = null
      this.#api = null
      try { this.#unsubscribe?.() } catch (caught) { error ??= caught }
      this.#unsubscribe = null
      try { await transport?.disconnect?.() } catch (caught) { error ??= caught }
      this.#state = {}
      this.#emit('state', { state: 'disconnected', reason: reason.code ?? 'CONNECTION_CLOSED' })
    }
    if (error) throw error
  }

  disconnect() {
    if (this.#disconnecting) return this.#disconnecting
    if (!this.#session) return Promise.resolve()
    const done = this.#dispose()
    this.#disconnecting = done.finally(() => { this.#disconnecting = null })
    return this.#disconnecting
  }

  inspect(request) { return this.#invoke('operations', 'inspect', request) }
  configure(request) { return this.#invoke('operations', 'configure', request) }
  control(request) { return this.#invoke('operations', 'control', request) }
  capture(request) { return this.#invoke('operations', 'capture', request) }
}

export function createDevice(options) { return new Device(options) }

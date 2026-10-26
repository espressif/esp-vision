/*
 * SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 *
 * UI-independent EV-MUX device interpreter. Both the browser IDE and agent
 * integrations construct this class with the same EvMuxParser/EvRepl
 * implementations, so protocol negotiation and firmware operations cannot
 * drift between the two entry points.
 */

export const EV_MUX_PAYLOAD_LIMIT = 1800
export const EV_MUX_SCRIPT_CHUNK_BYTES = 900
export const EV_MUX_VERSION = 4
const INLINE_PYTHON_REPL_TIMEOUT_MS = 5000
const SCRIPT_START_ACK_TIMEOUT_MS = 5000
// MicroPython has a 128-byte VFS path buffer; script.write also appends .tmp.
export const EV_MUX_PATH_MAX_BYTES = 123
export const EV_MUX_DEFAULT_LIMITS = Object.freeze({
  metadataMax: 384,
  rxPayloadMax: 2048,
  replInputMax: 511,
  vmPendingMax: 1,
  txLargePayloadMax: 0,
  txFrameTimeoutMs: 1500,
  txQueueTimeoutMs: 2000,
})

export const EV_MUX_REQUIRED_FEATURES = [
  'capabilities',
  'route.changed',
  'framed.repl',
  'session.firstCome',
  'session.keepalive',
]

export const EV_MUX_REQUIRED_CHANNELS = [
  'user.rpc',
  'debug.rpc',
  'repl.stdin',
  'repl.stdout',
  'repl.stderr',
  'repl.signal',
  'log.idf',
  'preview.frame',
]

const EV_MUX_USER_RPC_METHODS = new Set([
  'hello',
  'capabilities',
  'session.keepalive',
  'session.release',
  'file.write',
  'script.write',
  'script.run',
  'device.control',
])

function randomHex(bytes = 16) {
  const values = new Uint8Array(bytes)
  if (globalThis.crypto?.getRandomValues) {
    globalThis.crypto.getRandomValues(values)
  } else {
    for (let i = 0; i < values.length; i += 1) values[i] = Math.floor(Math.random() * 256)
  }
  return Array.from(values, (value) => value.toString(16).padStart(2, '0')).join('')
}

function utf8Bytes(value) {
  return new TextEncoder().encode(value)
}

function decodeRpcPayload(payload) {
  if (payload instanceof Uint8Array) payload = new TextDecoder('utf-8', { fatal: false }).decode(payload)
  if (typeof payload !== 'string') return payload && typeof payload === 'object' ? payload : null
  try {
    const value = JSON.parse(payload)
    return value && typeof value === 'object' && !Array.isArray(value) ? value : null
  } catch (_err) {
    return null
  }
}

function validateLimit(limits, name) {
  const value = limits?.[name]
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`EV-MUX v4 capabilities are missing a valid limits.${name}`)
  }
  return value
}

function responseVersion(payload) {
  return payload?.protocol?.evMuxVersion ?? payload?.evMuxVersion
}

function unsupportedVersion(version) {
  const error = new Error(`Unsupported EV-MUX protocol: this host requires EV-MUX v4 firmware, but the device reported ${version ?? 'an unknown version'}. Install firmware that explicitly supports EV-MUX v4.`)
  error.code = 'VERSION_MISMATCH'
  return error
}

function validateRpcPath(path) {
  if (typeof path !== 'string' || path.length === 0) {
    const error = new Error('Device RPC path must be a non-empty string')
    error.code = 'INVALID_PATH'
    throw error
  }
  if (utf8Bytes(path).length > EV_MUX_PATH_MAX_BYTES) {
    const error = new Error(`Device RPC path exceeds the firmware limit of ${EV_MUX_PATH_MAX_BYTES} UTF-8 bytes`)
    error.code = 'PATH_TOO_LONG'
    throw error
  }
  const forbiddenCharacter = Array.from(path).some((character) => (
    character.charCodeAt(0) < 0x20 || character.charCodeAt(0) === 0x7f
      || character === '\\' || character === '"'
  ))
  if (path.includes('..') || path.includes('//') || forbiddenCharacter) {
    const error = new Error('Device RPC path contains characters rejected by the firmware')
    error.code = 'INVALID_PATH'
    throw error
  }
}

function isVmEpoch(value) {
  return Number.isSafeInteger(value) && value >= 0 && value <= 0xffffffff
}

function isPositiveVmEpoch(value) {
  return isVmEpoch(value) && value > 0
}

function isVmEpochNewer(candidate, current) {
  if (!isVmEpoch(candidate) || !isVmEpoch(current) || candidate === current) return false
  // Zero is the pre-VM bootstrap generation. The firmware counter skips zero
  // when it wraps from UINT32_MAX back to one.
  if (current === 0) return candidate > 0
  if (candidate === 0) return false
  const distance = (candidate - current) >>> 0
  return distance > 0 && distance < 0x80000000
}

function isVmEpochAtLeast(candidate, current) {
  return isVmEpoch(candidate)
    && (current === null || candidate === current || isVmEpochNewer(candidate, current))
}

function isNetworkAction(method, payload) {
  if (method !== 'device.control') return false
  return typeof payload?.action === 'string' && payload.action.startsWith('network.')
}

function isVmRpc(method, payload) {
  if (method === 'file.write' || method === 'script.write' || method === 'script.run' || method === 'debug.capture_frame' || method === 'debug.capture_screen') return true
  if (method === 'debug.info') return payload?.scope !== 'transport.stats'
  return false
}

function payloadBytes(payload) {
  return payload instanceof Uint8Array ? payload : utf8Bytes(payload)
}

function defaultSleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms))
}

function abortError(signal, fallback = 'Device operation cancelled') {
  const reason = signal?.reason
  if (reason instanceof Error) return reason
  const error = new Error(typeof reason === 'string' ? reason : fallback)
  error.name = 'AbortError'
  return error
}

function awaitWithSignal(promise, signal) {
  if (!signal) return Promise.resolve(promise)
  if (signal.aborted) return Promise.reject(abortError(signal))
  return new Promise((resolve, reject) => {
    const cleanup = () => signal.removeEventListener('abort', onAbort)
    const onAbort = () => {
      cleanup()
      reject(abortError(signal))
    }
    signal.addEventListener('abort', onAbort, { once: true })
    Promise.resolve(promise).then((value) => {
      cleanup()
      resolve(value)
    }, (error) => {
      cleanup()
      reject(error)
    })
  })
}

function createOperationDeadline(signal, timeout, timeoutError = Object.assign(new Error('Python execution timed out'), { code: 'TIMEOUT' })) {
  if (timeout === null) return { signal, dispose() {} }
  const controller = new AbortController()
  const abortFromCaller = () => controller.abort(abortError(signal))
  signal?.addEventListener('abort', abortFromCaller, { once: true })
  if (signal?.aborted) abortFromCaller()
  const timer = controller.signal.aborted
    ? null
    : setTimeout(() => controller.abort(timeoutError), timeout)
  return {
    signal: controller.signal,
    dispose() {
      clearTimeout(timer)
      signal?.removeEventListener('abort', abortFromCaller)
    },
  }
}

function withTimeout(operation, timeout, message, signal = null) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(abortError(signal))
      return
    }
    let settled = false
    const finish = (callback, value) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      signal?.removeEventListener('abort', onAbort)
      callback(value)
    }
    const timer = setTimeout(() => {
      const error = new Error(message)
      error.code = 'WRITE_TIMEOUT'
      finish(reject, error)
    }, timeout)
    const onAbort = () => finish(reject, abortError(signal))
    signal?.addEventListener('abort', onAbort, { once: true })
    Promise.resolve().then(operation).then(
      (value) => finish(resolve, value),
      (error) => finish(reject, error),
    )
  })
}

export function validateEvmuxCapabilities(payload) {
  if (!payload || payload.ok !== true) {
    throw new Error('Missing EV-MUX capabilities')
  }
  const version = responseVersion(payload)
  if (version !== EV_MUX_VERSION) {
    throw unsupportedVersion(version)
  }

  const features = new Set(payload.features || [])
  const missing = EV_MUX_REQUIRED_FEATURES.filter((feature) => !features.has(feature))
  if (missing.length) {
    throw new Error(`Firmware is missing EV-MUX features: ${missing.join(', ')}. Please upgrade the ESP Vision firmware.`)
  }

  const channels = new Set(payload.channels || [])
  for (const channel of EV_MUX_REQUIRED_CHANNELS) {
    if (!channels.has(channel)) {
      throw new Error(`Firmware is missing EV-MUX channel: ${channel}. Please upgrade the ESP Vision firmware.`)
    }
  }

  for (const key of Object.keys(EV_MUX_DEFAULT_LIMITS)) validateLimit(payload.limits, key)
  if (payload.features.includes('file.write.binary') && (!Number.isSafeInteger(payload.limits.fileWriteChunkMax)
      || payload.limits.fileWriteChunkMax < 4096 || payload.limits.fileWriteChunkMax > 16384
      || payload.limits.fileWriteChunkMax % 4096 !== 0)) {
    throw new Error('EV-MUX v4 capabilities are missing a valid limits.fileWriteChunkMax')
  }
  if (payload.session?.policy !== 'firstCome'
      || !Number.isSafeInteger(payload.session.leaseMs)
      || !Number.isSafeInteger(payload.session.keepaliveMs)
      || payload.session.keepaliveMs <= 0
      || payload.session.leaseMs < payload.session.keepaliveMs * 2) {
    throw new Error('EV-MUX v4 capabilities are missing a valid first-come session lease')
  }
}

export function requiresEvmuxFirmwareUpgrade(err) {
  const message = err?.message || ''
  return err?.code === 'VERSION_MISMATCH'
    || message === 'EV-MUX hello timeout'
    || message === 'EV-MUX capabilities timeout'
    || message === 'Missing EV-MUX capabilities'
    || message.startsWith('Unsupported EV-MUX protocol:')
    || message.startsWith('Firmware is missing EV-MUX ')
    || message.startsWith('EV-MUX v4 capabilities are missing ')
}

export function evmuxRpcChannelForMethod(method) {
  return EV_MUX_USER_RPC_METHODS.has(method) ? 'user.rpc' : 'debug.rpc'
}

function evmuxLogicalSidForChannel(channel) {
  return channel === 'debug.rpc' ? 'debug' : 'user'
}

export class DeviceSession {
  constructor({
    transport,
    EvMuxParser,
    makeEvMuxFrame,
    EvRepl,
    handlers = {},
    sid = 'ide-client',
    sleep = defaultSleep,
  }) {
    if (!transport?.writeBytes) {
      throw new Error('DeviceSession requires a byte transport')
    }
    if (!EvMuxParser || !makeEvMuxFrame || !EvRepl) {
      throw new Error('DeviceSession requires the shared EV-MUX parser and REPL interpreter')
    }

    this.transport = transport
    this.makeEvMuxFrame = makeEvMuxFrame
    this.handlers = handlers
    this.sid = sid
    this.sleep = sleep
    this.sequence = 1
    this.clientNonce = randomHex()
    this.state = 'disconnected'
    this.role = 'unknown'
    this.capabilities = null
    this.identity = null
    this.limits = { ...EV_MUX_DEFAULT_LIMITS }
    this.sessionId = null
    this.vmEpoch = null
    this.vmReady = false
    this.vmReadyKnown = false
    this.keepaliveMs = 5000
    this.leaseMs = 15000
    this.keepaliveTimer = null
    this.keepaliveGeneration = 0
    this.sessionGeneration = 0
    this.negotiatePromise = null
    this.negotiateController = null
    this.releasePromise = null
    this.resetPromise = null
    this.vmReadinessProbeActive = false
    this.pendingRpc = new Map()
    this.draining = false
    this.lastDebugImage = null
    this.runCapture = null
    this.runStarting = false

    this.repl = new EvRepl({
      writeEvmux: (channel, type, method, payload, options) => this.writeFrame(channel, type, method, payload, options),
    })
    this.repl.onScriptEnd = () => this._handleScriptEnd()

    this.parser = new EvMuxParser({
      filterFrame: (metadata, payload) => this._acceptInboundFrame(metadata, payload),
      text: (text) => {
        const passthrough = this.repl.feedStdout(text)
        if (passthrough) {
          if (this.runCapture) {
            const remaining = this.runCapture.maxOutputChars - this.runCapture.output.length
            if (remaining > 0) this.runCapture.output += passthrough.slice(0, remaining)
            if (passthrough.length > remaining) this.runCapture.outputTruncated = true
          }
          this.handlers.text?.(passthrough)
        }
      },
      rawText: (text) => this.handlers.rawText?.(text),
      preview: (base64, metadata, bytes) => this.handlers.preview?.(base64, metadata, bytes),
      debugImage: (base64, metadata, bytes, frameMetadata) => {
        this.lastDebugImage = { base64, metadata }
        const pending = this.pendingRpc.get(Number(metadata.seq))
        if (pending?.binaryResponse && frameMetadata && this._matchesPendingResponse(frameMetadata, { ok: true })) {
          pending.image = { data: bytes.slice(), mimeType: 'image/jpeg', metadata: { ...metadata } }
        }
        this.handlers.debugImage?.(base64, metadata)
      },
      log: (text) => this.handlers.log?.(text),
      command: (text) => this.handlers.command?.(text),
      other: (text) => this.handlers.other?.(text),
      rpc: (metadata, payload) => this._handleRpc(metadata, payload),
    })
  }

  _diagnostic(event, details = {}) {
    this.handlers.diagnostic?.({ event, ...details })
  }

  _matchesPendingResponse(metadata, payload) {
    const seq = Number(metadata.seq)
    const pending = this.pendingRpc.get(seq)
    if (!pending || pending.channel !== metadata.channel || pending.method !== metadata.method) return false

    const response = decodeRpcPayload(payload)
    if (pending.sessionId) {
      if (response?.ok === false && metadata.sessionId === undefined
          && metadata.vmEpoch === undefined
          && ['SESSION_REQUIRED', 'SESSION_EXPIRED', 'NOT_OWNER'].includes(response.error?.code)) return true
      if (metadata.sessionId !== pending.sessionId) return false
      if (pending.vmRequest) {
        if (!isPositiveVmEpoch(metadata.vmEpoch) || metadata.vmEpoch !== pending.vmEpoch) return false
      } else if (!isVmEpoch(metadata.vmEpoch)) {
        return false
      }
      if (pending.method === 'session.keepalive' && response?.ok === true) {
        if (!isVmEpoch(response.vmEpoch) || response.vmEpoch !== metadata.vmEpoch) return false
        if (response.sessionId !== undefined && response.sessionId !== pending.sessionId) return false
      }
    } else if (pending.acquireSession) {
      if (response?.ok === false) {
        return metadata.sessionId === undefined && metadata.vmEpoch === undefined
      }
      // Correlate an older firmware's hello only to report a version error.
      // It must never become an acquired session or enable unowned frames.
      const version = responseVersion(response)
      if (response?.ok === true && Number.isSafeInteger(version) && version !== EV_MUX_VERSION) {
        return metadata.sessionId === undefined && metadata.vmEpoch === undefined
      }
      const responseSessionId = response?.session?.id
      const responseEpoch = response?.session?.vmEpoch
      if (!/^[0-9a-f]{32}$/.test(responseSessionId || '')
          || metadata.sessionId !== responseSessionId
          || !isVmEpoch(responseEpoch)
          || metadata.vmEpoch !== responseEpoch) return false
    } else if (metadata.sessionId !== undefined || metadata.vmEpoch !== undefined) {
      // Discovery responses must not carry an owner's lease or VM generation.
      return false
    }
    return true
  }

  _acceptInboundFrame(metadata, payload) {
    if (this.draining) return false
    const channel = metadata.channel || ''
    const type = metadata.type || ''
    if (channel === 'log.idf') return true

    if (channel === 'user.rpc' || channel === 'debug.rpc') {
      if (type === 'rsp') return this._matchesPendingResponse(metadata, payload)
      if (type !== 'event') return false

      const eventPayload = decodeRpcPayload(payload)
      if (metadata.method === 'hello') {
        if (!this.sessionId || responseVersion(eventPayload) !== EV_MUX_VERSION) return false
        const eventSessionId = eventPayload?.session?.id
        const sessionEpoch = eventPayload?.session?.vmEpoch
        const payloadEpoch = eventPayload?.vmEpoch
        return metadata.sessionId === this.sessionId
          && eventSessionId === this.sessionId
          && isVmEpoch(sessionEpoch)
          && sessionEpoch === payloadEpoch
          && payloadEpoch === metadata.vmEpoch
          && isVmEpochAtLeast(sessionEpoch, this.vmEpoch)
      }

      if (!this.sessionId) return false
      if (metadata.sessionId !== this.sessionId || !isVmEpoch(metadata.vmEpoch)) return false
      // Route and network notifications are transport events. They remain
      // valid across VM restarts but still belong to the active owner session.
      // Their epoch is never used to advance or roll back the session epoch.
      return Boolean(metadata.method)
    }

    if (channel === 'repl.stdout' || channel === 'repl.stderr') {
      return Boolean(this.sessionId)
        && (this.vmReady || this.vmReadinessProbeActive)
        && isPositiveVmEpoch(this.vmEpoch)
        && metadata.sessionId === this.sessionId
        && metadata.vmEpoch === this.vmEpoch
    }

    if (channel === 'preview.frame') {
      return Boolean(this.sessionId)
        && this.vmReady
        && isPositiveVmEpoch(this.vmEpoch)
        && metadata.sessionId === this.sessionId
        && metadata.vmEpoch === this.vmEpoch
    }

    return false
  }

  _handleRpc(metadata, payload) {
    const seq = Number(metadata.seq)
    const channel = metadata.channel || 'user.rpc'
    let requestPayload = null
    let callbackPayload = payload
    if (metadata.type === 'event') {
      if (metadata.method === 'hello') this._handleHelloEvent(payload)
      callbackPayload = metadata.method === 'network.changed'
        ? this._redactRpcPayload('device.control', { action: 'network.status', ...payload })
        : payload
      this.handlers.rpcEvent?.({ channel, metadata, payload: callbackPayload })
    } else if (metadata.type === 'rsp') {
      const pending = this.pendingRpc.get(seq)
      if (pending && this._matchesPendingResponse(metadata, payload)) {
        requestPayload = pending.requestPayload
        this.pendingRpc.delete(seq)
        pending.resolve({ metadata, payload, image: pending.image })
      } else {
        this.handlers.rpcOrphan?.({
          channel,
          metadata,
          payload: this._redactRpcPayload(metadata.method, payload, {
            forceNetwork: metadata.method === 'device.control',
          }),
        })
      }
      callbackPayload = this._redactRpcPayload(metadata.method, payload, {
        requestPayload,
        forceNetwork: metadata.method === 'device.control' && requestPayload === null,
      })
    }
    this.handlers.rpc?.({
      channel,
      metadata,
      payload: callbackPayload,
    })
    return false
  }

  _handleHelloEvent(payload) {
    if (!payload || responseVersion(payload) !== EV_MUX_VERSION) return
    const eventSession = payload.session
    const epoch = eventSession?.vmEpoch
    if (!this.sessionId || eventSession?.id !== this.sessionId) return
    if (!isVmEpoch(epoch) || epoch !== payload.vmEpoch || !isVmEpochAtLeast(epoch, this.vmEpoch)) return
    this._updateVmEpoch(epoch, 'device soft reset')
    if (typeof payload.vmReady === 'boolean') this._setVmReady(payload.vmReady)
  }

  _updateVmEpoch(epoch, reason = 'VM epoch changed') {
    if (!isVmEpoch(epoch)) return false
    if (this.vmEpoch !== null && epoch !== this.vmEpoch && !isVmEpochNewer(epoch, this.vmEpoch)) return false
    if (this.vmEpoch === epoch) return false
    const previous = this.vmEpoch
    this.vmEpoch = epoch
    this.vmReady = false
    if (previous !== null) {
      const error = new Error(`${reason}; pending VM requests were cleared`)
      error.code = 'VM_EPOCH_CHANGED'
      for (const [seq, pending] of this.pendingRpc) {
        if (!pending.vmRequest) continue
        this.pendingRpc.delete(seq)
        pending.reject(error)
      }
      this.runCapture?.reject(error)
      if (this.runCapture) {
        clearTimeout(this.runCapture.timer)
        this.runCapture = null
        this.repl.cancelScriptEndSentinel(error)
      }
      this.parser.resetReplTextDecoders?.()
      this.repl.resetEpoch(error)
      this._diagnostic('session.vm_epoch_changed', { previous, current: epoch })
    }
    return true
  }

  _setVmReady(ready) {
    this.vmReadyKnown = true
    this.vmReady = ready === true && isPositiveVmEpoch(this.vmEpoch)
    return this.vmReady
  }

  async _keepaliveOnce({ sessionId, generation, keepaliveGeneration }) {
    try {
      return await this.rpc('session.keepalive', {}, { timeout: 3000 })
    } catch (error) {
      // Only lease renewal is replayable. Retain one 3 s attempt within the
      // negotiated lease; never retry a business command or transfer an owner.
      if (error.code !== 'TIMEOUT' || this.leaseMs <= this.keepaliveMs + 6000
          || this.sessionId !== sessionId || this.sessionGeneration !== generation
          || this.keepaliveGeneration !== keepaliveGeneration || this.state === 'disconnected') throw error
      this._diagnostic('session.keepalive_retry', { code: error.code })
      return this.rpc('session.keepalive', {}, { timeout: 3000 })
    }
  }

  _startKeepalive() {
    clearTimeout(this.keepaliveTimer)
    this.keepaliveGeneration += 1
    if (!this.sessionId || this.keepaliveMs <= 0) return
    const sessionId = this.sessionId
    const generation = this.sessionGeneration
    const keepaliveGeneration = this.keepaliveGeneration
    const tick = async () => {
      if (this.sessionId !== sessionId || this.sessionGeneration !== generation
          || this.keepaliveGeneration !== keepaliveGeneration || this.state === 'disconnected') return
      try {
        const response = await this._keepaliveOnce({ sessionId, generation, keepaliveGeneration })
        if (this.sessionId !== sessionId || this.sessionGeneration !== generation
            || this.keepaliveGeneration !== keepaliveGeneration) return
        if (response.sessionId && response.sessionId !== sessionId) {
          throw new Error('EV-MUX keepalive returned a different session ID')
        }
        const responseEpoch = isVmEpoch(response.vmEpoch) ? response.vmEpoch : this.vmEpoch
        this._updateVmEpoch(responseEpoch, 'VM epoch changed')
        if (responseEpoch === this.vmEpoch && typeof response.vmReady === 'boolean') {
          this._setVmReady(response.vmReady)
        }
      } catch (error) {
        if (this.sessionId === sessionId && this.sessionGeneration === generation
            && this.keepaliveGeneration === keepaliveGeneration) {
          this._diagnostic('session.keepalive_failed', { code: error.code || 'ERROR', message: error.message })
          this.close(error)
          this.handlers.sessionLost?.(error)
        }
        return
      }
      if (this.sessionId !== sessionId || this.sessionGeneration !== generation
          || this.keepaliveGeneration !== keepaliveGeneration || this.state === 'disconnected') return
      this.keepaliveTimer = setTimeout(tick, this.keepaliveMs)
      this.keepaliveTimer.unref?.()
    }
    this.keepaliveTimer = setTimeout(tick, this.keepaliveMs)
    this.keepaliveTimer.unref?.()
  }

  _allocateSequence() {
    for (let attempt = 0; attempt < 0x100000000; attempt += 1) {
      const seq = this.sequence >>> 0
      this.sequence = (seq + 1) >>> 0
      if (!this.pendingRpc.has(seq)) return seq
    }
    throw new Error('EV-MUX request sequence space is exhausted')
  }

  _redactRpcPayload(method, payload, {
    requestPayload = null,
    redactSsid = true,
    forceNetwork = false,
  } = {}) {
    if (payload instanceof Uint8Array) return { binaryBytes: payload.byteLength }
    if (!forceNetwork && !isNetworkAction(method, payload) && !isNetworkAction(method, requestPayload)) return payload
    const secrets = [requestPayload?.ssid, requestPayload?.password]
      .filter((value) => typeof value === 'string' && value.length > 0)
    const sanitize = (value, key = '') => {
      if (key.toLowerCase() === 'password' || (redactSsid && key.toLowerCase() === 'ssid')) {
        return '[redacted]'
      }
      if (key.toLowerCase() === 'message') {
        return 'Wi-Fi operation failed. Use the error code to diagnose it.'
      }
      if (typeof value === 'string') {
        return secrets.reduce((result, secret) => result.split(secret).join('[redacted]'), value)
      }
      if (Array.isArray(value)) return value.map((item) => sanitize(item))
      if (value && typeof value === 'object') {
        return Object.fromEntries(Object.entries(value).map(([childKey, childValue]) => [
          childKey,
          sanitize(childValue, childKey),
        ]))
      }
      return value
    }
    return sanitize(payload)
  }

  _handleScriptEnd() {
    const capture = this.runCapture
    if (!capture) {
      this.handlers.scriptEnd?.()
      return
    }
    clearTimeout(capture.timer)
    // feedStdout reports the marker before returning the rest of this frame.
    // Keep capture active until the parser has accumulated that final output.
    queueMicrotask(() => {
      if (this.runCapture !== capture) return
      this.runCapture = null
      this.handlers.scriptEnd?.()
      capture.resolve({
        ok: true,
        path: capture.path,
        output: capture.output.replace(/\r/g, ''),
        outputTruncated: capture.outputTruncated,
      })
    })
  }

  feedBytes(bytes) {
    this.parser.feedBytes(bytes)
  }

  feedText(text) {
    this.parser.feedText(text)
  }

  hasFeature(feature) {
    return Boolean(this.capabilities?.features?.includes(feature))
  }

  async writeFrame(channel, type, method, payload, {
    authorize = null,
    vmRequest = null,
    seq: requestedSeq = null,
    signal = null,
    writeTimeout = null,
    encoding = undefined,
  } = {}) {
    if (this.state === 'disconnected') {
      throw new Error('EV-MUX is not connected')
    }
    const seq = requestedSeq === null ? this._allocateSequence() : requestedSeq >>> 0
    const requestPayload = typeof payload === 'string' && method
      ? (() => { try { return JSON.parse(payload) } catch (_err) { return {} } })()
      : payload
    const needsAuthorization = authorize ?? !(method === 'hello' || method === 'capabilities')
    const usesVm = vmRequest ?? (channel === 'repl.stdin' || channel === 'repl.signal'
      || (Boolean(method) && isVmRpc(method, requestPayload)))
    if (this.state === 'resetting' && !(method === 'device.control' && requestPayload?.action === 'reset')) {
      throw Object.assign(new Error('Device reset is in progress'), { code: 'DEVICE_RESETTING' })
    }
    if (needsAuthorization && !this.sessionId) {
      throw new Error(`EV-MUX ${method || channel} requires an acquired v4 session`)
    }
    const readinessProbe = this.vmReadinessProbeActive
    if (usesVm && needsAuthorization) {
      if (!isPositiveVmEpoch(this.vmEpoch)) {
        throw new Error(`EV-MUX ${method || channel} requires a positive VM epoch`)
      }
      if (!this.vmReady && !readinessProbe) {
        throw new Error(`EV-MUX ${method || channel} requires a ready VM`)
      }
    }
    const payloadData = payloadBytes(payload)
    if (channel === 'repl.signal'
        && (payloadData.length !== 1 || payloadData[0] !== 0x03)) {
      throw new Error('EV-MUX repl.signal only accepts the single-byte Ctrl-C signal 0x03')
    }
    const payloadLimit = method === 'file.write' && encoding === 'binary' && this.hasFeature('file.write.binary')
      ? this.limits.fileWriteChunkMax + 20 : this.limits.rxPayloadMax
    if (this.capabilities && payloadData.length > payloadLimit) {
      throw new Error(`EV-MUX payload is ${payloadData.length} bytes; device limit is ${this.limits.rxPayloadMax}`)
    }
    if ((channel === 'repl.stdin' || channel === 'repl.signal') && payloadData.length > this.limits.replInputMax) {
      throw new Error(`EV-MUX REPL input is ${payloadData.length} bytes; device limit is ${this.limits.replInputMax}`)
    }
    const frameType = channel === 'repl.stdin' || channel === 'repl.signal' ? 'req' : type
    const metadata = {
      sid: evmuxLogicalSidForChannel(channel),
      seq,
      channel,
      type: frameType,
      encoding: encoding ?? (channel.endsWith('.rpc') ? 'json' : 'text'),
      ...(method ? { method } : {}),
      ...(needsAuthorization ? { sessionId: this.sessionId } : {}),
      ...(usesVm && needsAuthorization ? { vmEpoch: this.vmEpoch } : {}),
    }
    if (utf8Bytes(JSON.stringify(metadata)).length > this.limits.metadataMax) {
      throw new Error(`EV-MUX metadata exceeds device limit ${this.limits.metadataMax}`)
    }
    const frame = this.makeEvMuxFrame({
      channel,
      type: frameType,
      encoding,
      method,
      payload,
      seq,
      sid: evmuxLogicalSidForChannel(channel),
      sessionId: needsAuthorization ? this.sessionId : undefined,
      vmEpoch: usesVm && needsAuthorization ? this.vmEpoch : undefined,
    })
    const boundedWriteTimeout = Math.max(1, Math.min(
      writeTimeout ?? Number.MAX_SAFE_INTEGER,
      (this.limits.txFrameTimeoutMs || EV_MUX_DEFAULT_LIMITS.txFrameTimeoutMs)
        + (this.limits.txQueueTimeoutMs || EV_MUX_DEFAULT_LIMITS.txQueueTimeoutMs),
    ))
    await withTimeout(
      () => this.transport.writeBytes(frame),
      boundedWriteTimeout,
      'EV-MUX transport write timeout',
      signal,
    )
    return seq
  }

  async rpc(method, payload = {}, { timeout = 4000, channel = null, signal = null, binaryResponse = false } = {}) {
    if (this.state === 'disconnected') {
      throw new Error('EV-MUX is not connected')
    }
    if (signal?.aborted) throw abortError(signal)
    if (payload instanceof Uint8Array && method !== 'file.write') throw new TypeError('Raw binary requests are only supported for file.write')
    if (method === 'script.write' || method === 'script.run') validateRpcPath(payload?.path)
    if (method === 'debug.info' && (payload?.scope === 'fs.list' || payload?.scope === 'fs.read')) {
      validateRpcPath(payload.path ?? (payload.scope === 'fs.list' ? '/' : undefined))
    }

    const rpcChannel = channel || evmuxRpcChannelForMethod(method)
    const seq = this._allocateSequence()
    const vmRequest = isVmRpc(method, payload)
    const response = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingRpc.delete(seq)
        cleanup()
        this.handlers.rpcTimeout?.({ channel: rpcChannel, method, seq, timeout })
        const error = new Error(`EV-MUX ${method} timeout`)
        error.code = 'TIMEOUT'
        reject(error)
      }, timeout)
      const onAbort = () => {
        this.pendingRpc.delete(seq)
        clearTimeout(timer)
        cleanup()
        reject(abortError(signal))
      }
      signal?.addEventListener('abort', onAbort, { once: true })
      const cleanup = () => signal?.removeEventListener('abort', onAbort)
      this.pendingRpc.set(seq, {
        method,
        channel: rpcChannel,
        vmRequest,
        sessionId: method === 'hello' || method === 'capabilities' ? null : this.sessionId,
        vmEpoch: method === 'hello' || method === 'capabilities' ? null : this.vmEpoch,
        acquireSession: method === 'hello' && payload?.acquire === true,
        requestPayload: payload,
        binaryResponse,
        timer,
        resolve: (value) => {
          clearTimeout(timer)
          cleanup()
          resolve(value)
        },
        reject: (error) => {
          clearTimeout(timer)
          cleanup()
          reject(error)
        },
      })
    })

    const safePayload = this._redactRpcPayload(method, payload)
    this.handlers.rpcRequest?.({ channel: rpcChannel, method, seq, timeout, payload: safePayload })
    this._diagnostic('rpc.request', { channel: rpcChannel, method, seq })
    try {
      await this.writeFrame(rpcChannel, 'req', method, payload instanceof Uint8Array ? payload : JSON.stringify(payload), {
        encoding: payload instanceof Uint8Array ? 'binary' : undefined,
        vmRequest,
        seq,
        signal,
        writeTimeout: timeout,
      })
    } catch (err) {
      const pending = this.pendingRpc.get(seq)
      this.pendingRpc.delete(seq)
      pending?.reject(err)
      this.handlers.rpcWriteError?.({ channel: rpcChannel, method, seq, error: err })
      try {
        await response
      } catch (_err) {
        // The original transport error below is more useful.
      }
      throw err
    }

    const { metadata, payload: responsePayload, image } = await response
    const safeResponse = this._redactRpcPayload(method, responsePayload, { requestPayload: payload })
    this.handlers.rpcResponse?.({ channel: rpcChannel, method, seq, metadata, payload: safeResponse })
    this._diagnostic('rpc.response', { channel: rpcChannel, method, seq, ok: responsePayload?.ok })
    if (!responsePayload?.ok) {
      const safeError = this._redactRpcPayload(method, responsePayload, { requestPayload: payload })
      const error = new Error(safeError?.error?.message || `${method} failed`)
      error.code = responsePayload?.error?.code || 'RPC_ERROR'
      error.response = safeError
      throw error
    }
    if (binaryResponse) {
      if (!image) throw Object.assign(new Error('Capture returned no binary image'), { code: 'INVALID_RESPONSE' })
      return image
    }
    return this._redactRpcPayload(method, responsePayload, { requestPayload: payload, redactSsid: false })
  }

  captureImage(method, payload = {}, options = {}) {
    return this.rpc(method, payload, { ...options, channel: 'debug.rpc', binaryResponse: true })
  }

  negotiate(options = {}) {
    if (this.negotiatePromise) return this.negotiatePromise
    if (this.releasePromise) return Promise.reject(new Error('EV-MUX session is being released'))
    if (this.resetPromise) {
      return Promise.reject(Object.assign(new Error('Device reset is in progress'), { code: 'DEVICE_RESETTING' }))
    }

    const controller = new AbortController()
    const callerSignal = options.signal || null
    const abortFromCaller = () => controller.abort(callerSignal.reason)
    if (callerSignal?.aborted) abortFromCaller()
    else callerSignal?.addEventListener('abort', abortFromCaller, { once: true })
    this.negotiateController = controller
    const deadlineError = new Error('EV-MUX negotiation deadline exceeded')
    deadlineError.code = 'TIMEOUT'
    const deadlineTimer = setTimeout(() => controller.abort(deadlineError), 30000)
    const operation = this._negotiate({ ...options, signal: controller.signal })
    const tracked = operation.finally(() => {
      clearTimeout(deadlineTimer)
      callerSignal?.removeEventListener('abort', abortFromCaller)
      if (this.negotiatePromise === tracked) this.negotiatePromise = null
      if (this.negotiateController === controller) this.negotiateController = null
      if (this.state === 'negotiating' && !this.sessionId) this.state = 'disconnected'
    })
    this.negotiatePromise = tracked
    return tracked
  }

  async _negotiate({ role = 'unknown', drainMs = 300, signal = null, acquire = true, probeRepl = true, expectedDevice = null } = {}) {
    this.role = role
    this.state = 'negotiating'
    this._diagnostic('negotiate.begin', { role })

    // Unclaimed TCP sockets expire in three seconds: acquire immediately.
    if (role === 'tcp') drainMs = 0
    if (drainMs > 0) {
      this.draining = true
      try {
        await awaitWithSignal(this.sleep(drainMs), signal)
      } finally {
        this.draining = false
      }
    }
    if (signal?.aborted) throw abortError(signal)
    if (drainMs > 0 && this.parser.hasPartialFrame?.()) {
      const error = Object.assign(new Error('Incomplete EV-MUX frame at end of startup drain'), {
        code: 'SERIAL_STARTUP_INCOMPLETE_FRAME',
      })
      this.close(error)
      this.parser.reset()
      await this.transport.disconnect?.()
      throw error
    }

    let capabilities
    let discovery
    let hello = null
    if (expectedDevice) {
      const candidate = await this.rpc('hello', {}, { timeout: 1500, signal })
      if (responseVersion(candidate) !== EV_MUX_VERSION) throw unsupportedVersion(responseVersion(candidate))
      if (candidate.deviceId !== expectedDevice.deviceId || candidate.hostname !== expectedDevice.hostname) {
        const error = new Error('Selected device identity does not match the connected firmware')
        error.code = 'DEVICE_IDENTITY_MISMATCH'
        throw error
      }
    }
    if (acquire) {
      const acquirePayload = {
        evMuxVersion: EV_MUX_VERSION,
        acquire: true,
        clientNonce: this.clientNonce,
      }
      const acquireDeadline = Date.now() + 4000
      let timeoutReplayAttempted = false
      while (true) {
        const remainingMs = acquireDeadline - Date.now()
        if (remainingMs <= 0) {
          const error = new Error('EV-MUX session acquisition deadline exceeded')
          error.code = 'TIMEOUT'
          throw error
        }
        try {
          hello = await this.rpc('hello', acquirePayload, {
            // Leave time to replay a lost acknowledgement with the same nonce.
            timeout: Math.min(timeoutReplayAttempted ? 4000 : 2000, remainingMs),
            signal,
            channel: 'user.rpc',
          })
          break
        } catch (error) {
          if (signal?.aborted) throw abortError(signal)
          if (error.code === 'TIMEOUT' && !timeoutReplayAttempted) {
            // The first response may have been lost after the device acquired
            // us. Reusing this nonce makes that replay idempotent on this link.
            timeoutReplayAttempted = true
            continue
          }
          if (error.code !== 'SESSION_RELEASING') throw error

          // The prior lease owner is finishing its release response on this
          // same link. Retry this nonce only within one bounded acquire window;
          // SESSION_BUSY still fails immediately because that link is closed.
          const retryDelayMs = Math.min(10, Math.max(0, acquireDeadline - Date.now()))
          if (retryDelayMs <= 0) throw error
          await awaitWithSignal(this.sleep(retryDelayMs), signal)
        }
      }
      if (responseVersion(hello) !== EV_MUX_VERSION) throw unsupportedVersion(responseVersion(hello))
      if (!hello.session || typeof hello.session.id !== 'string') {
        throw new Error('EV-MUX v4 acquire response did not include an active session')
      }
      if (!/^[0-9a-f]{32}$/.test(hello.session.id)
          || !isVmEpoch(hello.session.vmEpoch)) {
        throw new Error('EV-MUX v4 acquire response contains an invalid session ID or VM epoch')
      }
      if (this.sessionId !== hello.session.id) this.sessionGeneration += 1
      this.sessionId = hello.session.id
      this._updateVmEpoch(hello.session.vmEpoch)
      this.vmReadyKnown = typeof hello.vmReady === 'boolean'
      if (this.vmReadyKnown) this._setVmReady(hello.vmReady)
      this.keepaliveMs = hello.session.keepaliveMs || this.keepaliveMs
      this.leaseMs = hello.session.leaseMs || this.leaseMs
      this.parser.active = true
      this.state = 'ready'
      this._startKeepalive()
    }

    try {
      // Queue/frame TX limits are not an end-to-end network response budget.
      // Match the reference client's six-second ordinary RPC deadline.
      capabilities = await this.rpc('capabilities', {}, { timeout: 6000, signal })
      validateEvmuxCapabilities(capabilities)
      discovery = await this.rpc('hello', {}, { timeout: 6000, signal })
    } catch (err) {
      this._diagnostic('negotiate.discovery_failed', {
        parser: this.parser.getDiagnostics?.(),
        transport: this.transport.getDiagnostics?.(),
        sessionAcquired: Boolean(this.sessionId),
      })
      throw err
    }

    if (discovery.evMuxVersion !== EV_MUX_VERSION) {
      throw unsupportedVersion(responseVersion(discovery))
    }
    if (expectedDevice && (discovery.deviceId !== expectedDevice.deviceId
        || discovery.hostname !== expectedDevice.hostname)) {
      const error = new Error('Selected device identity does not match the connected firmware')
      error.code = 'DEVICE_IDENTITY_MISMATCH'
      throw error
    }
    this.identity = { deviceId: discovery.deviceId ?? null, hostname: discovery.hostname ?? null,
      board: discovery.board, firmwareVersion: discovery.firmware?.version }
    this.capabilities = capabilities
    this.limits = { ...EV_MUX_DEFAULT_LIMITS, ...capabilities.limits }
    this.keepaliveMs = capabilities.session.keepaliveMs
    this.leaseMs = capabilities.session.leaseMs
    this.repl.inputMaxBytes = this.limits.replInputMax
    this.parser.setLimits?.({
      metadataMax: this.limits.metadataMax,
      payloadMax: Math.max(this.limits.rxPayloadMax, this.limits.txLargePayloadMax),
      frameProgressTimeoutMs: 12000,
      frameTotalTimeoutMs: 12000,
    })

    if (acquire) {
      // Start with the lease timing from the accepted hello immediately, then
      // refresh its cadence once the full capability response is available.
      this._startKeepalive()
    } else {
      this.parser.active = true
      this.state = 'discovered'
      this._diagnostic('negotiate.discovered', { role })
      return { hello: discovery, discovery, capabilities }
    }

    // Hard-reset reconciliation must not send Ctrl-C or execute a liveness
    // probe: the fixed launcher may already be running the selected App.
    if (!probeRepl) {
      this.parser.active = true
      this.state = acquire ? 'ready' : 'discovered'
      this._diagnostic('negotiate.ready_readonly', { role })
      return { hello, discovery, capabilities }
    }

    const canUseReadinessBarrier = acquire
      && isPositiveVmEpoch(this.vmEpoch)
      && (!this.vmReadyKnown || this.vmReady)
    if (canUseReadinessBarrier) {
      // vmReady means the VM has started, even while boot/main is running.
      // Ordinary IDE connections still need a responsive friendly REPL.
      this.vmReadinessProbeActive = true
      try {
        await this.writeFrame('repl.signal', 'signal', null, new Uint8Array([0x03]), { signal })
        const liveness = await this.repl.execPy('print("EVIDE_BOOT_OK")', { timeout: 10000, signal })
        if (!liveness.includes('EVIDE_BOOT_OK')) {
          throw new Error('EV-MUX REPL liveness check failed')
        }
        if (!this.vmReadyKnown) this._setVmReady(true)
      } finally {
        this.vmReadinessProbeActive = false
      }
    } else if (acquire && !this.vmReady) {
      this._diagnostic('negotiate.vm_not_ready', { vmEpoch: this.vmEpoch })
    }

    this.parser.active = true
    this.state = acquire ? 'ready' : 'discovered'
    this._diagnostic('negotiate.ready', { role })
    return { hello, discovery, capabilities }
  }

  async writeFile(path, content, {
    encoding = 'utf-8',
    mode = 'overwrite',
    runAfterWrite = false,
    signal = null,
    onProgress = null,
  } = {}) {
    if (signal?.aborted) throw abortError(signal)
    validateRpcPath(path)
    if (!['utf-8', 'binary'].includes(encoding)) {
      throw new Error(`Unsupported device file encoding: ${encoding}`)
    }
    if (!['create', 'overwrite'].includes(mode)) {
      throw new Error(`Unsupported device file mode: ${mode}`)
    }
    if (encoding === 'binary' && runAfterWrite) {
      throw new Error('Binary device files cannot run after write')
    }
    const bytes = typeof content === 'string'
      ? new TextEncoder().encode(content)
      : content instanceof Uint8Array
        ? content
        : new Uint8Array(content)
    if (!this.hasFeature('file.write.binary')) {
      throw Object.assign(new Error('File upload requires firmware with file.write.binary; update the device firmware'), { code: 'UNSUPPORTED_OPERATION' })
    }
    const limit = this.limits.fileWriteChunkMax
    if (!Number.isSafeInteger(limit) || limit < 4096 || limit > 16384 || limit % 4096 !== 0 || bytes.length > 0xffffffff) {
      throw new Error('Invalid negotiated binary file transfer limit or file size')
    }
    const digest = new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256', bytes))
    if (signal?.aborted) throw abortError(signal)
    const sha256 = Array.from(digest, byte => byte.toString(16).padStart(2, '0')).join('')
    const uploadId = randomHex()
    let committed = false
    try {
      const begin = await this.rpc('file.write', { action: 'begin', path, mode, totalBytes: bytes.length, sha256, uploadId }, { timeout: 8000, signal })
      if (begin.uploadId !== uploadId || begin.chunkBytes !== limit) throw new Error('Invalid binary file transfer acknowledgement')
      const token = Uint8Array.from(begin.uploadId.match(/../g), byte => parseInt(byte, 16))
      for (let offset = 0; offset < bytes.length; offset += limit) {
        if (signal?.aborted) throw abortError(signal)
        const chunk = bytes.subarray(offset, Math.min(offset + limit, bytes.length))
        const payload = new Uint8Array(20 + chunk.length)
        payload.set(token)
        new DataView(payload.buffer).setUint32(16, offset, true)
        payload.set(chunk, 20)
        const response = await this.rpc('file.write', payload, { timeout: 8000, signal })
        if (response.writtenBytes !== offset + chunk.length) throw new Error('Incomplete binary file transfer acknowledgement')
        onProgress?.({ writtenBytes: offset + chunk.length, totalBytes: bytes.length })
      }
      const result = await this.rpc('file.write', { action: 'commit', uploadId: begin.uploadId }, { timeout: 8000, signal })
      if (result.complete !== true || result.bytes !== bytes.length) throw new Error('Incomplete binary file commit acknowledgement')
      committed = true
      if (bytes.length === 0) onProgress?.({ writtenBytes: 0, totalBytes: 0 })
      if (runAfterWrite) await this.rpc('script.run', { path }, { timeout: 8000, signal })
      return { ...result, path, runAfterWrite }
    } finally {
      if (!committed) {
        // Cancellation does not cancel cleanup, and cleanup cannot mask the primary error.
        await this.rpc('file.write', { action: 'abort', uploadId }, { timeout: 1500 }).catch(() => {})
      }
    }
  }

  async writeScript(path, content, { runAfterWrite = false, signal = null } = {}) {
    return this.writeFile(path, content, { encoding: 'utf-8', runAfterWrite, signal })
  }

  createOperationId() {
    return randomHex()
  }

  _networkOperationId(value) {
    const operationId = value ?? this.createOperationId()
    if (typeof operationId !== 'string' || !/^[0-9a-f]{32}$/.test(operationId)) {
      throw new TypeError('Network operationId must be a 32-character lowercase hexadecimal string')
    }
    return operationId
  }

  async networkStatus(options = {}) {
    if (!this.hasFeature('network.background')) {
      throw new Error('This firmware does not support background network management')
    }
    return this.rpc('device.control', { action: 'network.status' }, options)
  }

  async configureNetwork({ ssid, password, operationId = undefined }, options = {}) {
    if (!this.hasFeature('network.configure')) {
      throw new Error('This firmware does not support network configuration')
    }
    if (typeof ssid !== 'string' || typeof password !== 'string') {
      throw new TypeError('Network name and password must be strings')
    }
    const { operationId: optionOperationId, ...rpcOptions } = options
    return this.rpc('device.control', {
      action: 'network.configure',
      operationId: this._networkOperationId(operationId ?? optionOperationId),
      ssid,
      password,
    }, rpcOptions)
  }

  async setNetworkEnabled(enabled, options = {}) {
    if (typeof enabled !== 'boolean') throw new TypeError('Network enabled state must be a boolean')
    const { operationId, ...rpcOptions } = options
    return this.rpc('device.control', {
      action: 'network.enable',
      operationId: this._networkOperationId(operationId),
      enabled,
    }, rpcOptions)
  }

  async forgetNetwork(options = {}) {
    const { operationId, ...rpcOptions } = options
    return this.rpc('device.control', {
      action: 'network.forget',
      operationId: this._networkOperationId(operationId),
    }, rpcOptions)
  }

  release({ timeout = 1200 } = {}) {
    if (this.releasePromise) return this.releasePromise
    const negotiation = this.negotiatePromise
    const resetting = this.resetPromise
    const operation = (async () => {
      if (resetting) {
        // The reset caller owns its result. After an ACK or uncertain reset
        // there is no old lease to release; a rejected reset retains it.
        try { await resetting } catch (_resetError) { /* Keep reset failure with its caller. */ }
      }
      if (negotiation) {
        try {
          await negotiation
        } catch (_error) {
          // A failed REPL barrier can still leave an accepted lease to release.
        }
      }
      if (!this.sessionId) return { ok: true, accepted: false }
      try {
        return await this.rpc('session.release', {}, { timeout, channel: 'user.rpc' })
      } finally {
        this.close(new Error('EV-MUX session released'))
      }
    })()
    const tracked = operation.finally(() => {
      if (this.releasePromise === tracked) this.releasePromise = null
    })
    this.releasePromise = tracked
    return tracked
  }

  async executePython(source, { timeout = null, signal = null } = {}) {
    if (timeout !== null && (!Number.isFinite(timeout) || timeout <= 0)) {
      throw new RangeError('Python execution timeout must be positive or null')
    }
    if (signal?.aborted) throw abortError(signal)
    const operation = createOperationDeadline(signal, timeout)
    signal = operation.signal
    try {
      if (signal?.aborted) throw abortError(signal)
      const commandBytes = this.repl.execInputBytes
        ? this.repl.execInputBytes(source)
        : new TextEncoder().encode(`exec(${JSON.stringify(source)})\r\n`).length
      const inputLimit = this.repl.inputMaxBytes || 511
      if (commandBytes <= inputLimit) {
        const result = await this.runScript(null, source, {
          timeout: null, signal, inlineSource: true,
        })
        return result.output
      }
      if (!this.hasFeature('file.write.binary') || !this.hasFeature('script.run')) {
        throw new Error(
          `Python source requires ${commandBytes} REPL bytes, above the firmware limit ${inputLimit}; ` +
          'upgrade firmware with file.write.binary and script.run support.',
        )
      }

      const path = `/.__espvision_exec_${Date.now().toString(36)}_${this.sequence}.py`
      let primaryError = null
      let cleanupError = null
      let output = null
      this._diagnostic('python.execute.chunked', { path, sourceBytes: new TextEncoder().encode(source).length, commandBytes })
      try {
        const result = await this.runScript(path, source, {
          timeout: null, signal, inlineCompletion: true,
        })
        output = result.output
      } catch (error) {
        primaryError = error
      } finally {
        if (primaryError) await this.stopForRepl().catch(() => false)
        try {
          await this.repl.removeFile(path)
        } catch (error) {
          cleanupError = error
          this._diagnostic('python.execute.cleanup_failed', { path, error: error.message })
        }
      }
      if (primaryError) throw primaryError
      if (cleanupError) throw cleanupError
      return output
    } finally {
      operation.dispose()
    }
  }

  async interrupt() {
    if (this.hasFeature('device.control')) {
      return this.rpc('device.control', { action: 'stop' }, { timeout: 3000 })
    }
    await this.writeFrame('repl.signal', 'signal', null, new Uint8Array([0x03]))
    return { ok: true }
  }

  async control(operation, { timeout = 15000, signal = null } = {}) {
    if (operation === 'stop') return this.interrupt()
    if (signal?.aborted) throw abortError(signal)
    if (operation === 'hard-reset' && this.hasFeature('device.control')) {
      return this._hardReset({ timeout, signal })
    }
    const command = {
      'soft-reset': 'import machine; machine.soft_reset()\r\n',
      'hard-reset': 'import machine; machine.reset()\r\n',
      bootloader: 'import machine; machine.bootloader()\r\n',
    }[operation]
    if (!command) throw new Error(`Unsupported device control operation: ${operation}`)
    if (operation === 'soft-reset') return this._softReset(command, { timeout, signal })
    await this.writeFrame('repl.signal', 'signal', null, new Uint8Array([0x03]), { signal })
    await this.writeFrame('repl.stdin', 'data', null, command, { signal })
    await awaitWithSignal(this.sleep(100), signal)
    return { ok: true, operation, acknowledged: false }
  }

  _hardReset({ timeout, signal }) {
    if (!Number.isFinite(timeout) || timeout <= 0) throw new RangeError('Reset timeout must be positive')
    if (this.releasePromise) {
      throw Object.assign(new Error('Device session is being released'), { code: 'DEVICE_RELEASING' })
    }
    if (this.resetPromise || this.state !== 'ready') {
      throw Object.assign(new Error('Device is not ready for reset'), { code: 'DEVICE_RESETTING' })
    }
    const owner = this.sessionId
    const generation = this.sessionGeneration
    this.state = 'resetting'
    clearTimeout(this.keepaliveTimer)
    this.keepaliveTimer = null
    this.keepaliveGeneration += 1
    const operation = (async () => {
      try {
        const response = await this.rpc('device.control', { action: 'reset' }, { timeout, signal })
        // The ACK confirms acceptance, not a completed reboot. USB Serial/JTAG
        // can stay open across restart; never send release using its old lease.
        this.close(Object.assign(new Error('Device reset accepted'), { code: 'DEVICE_RESET' }))
        return response
      } catch (error) {
        const rejected = error.response?.ok === false
        const leaseLost = ['SESSION_REQUIRED', 'SESSION_EXPIRED', 'NOT_OWNER'].includes(error.code)
        if (rejected && !leaseLost && this.sessionId === owner && this.sessionGeneration === generation) {
          // An authenticated rejection proves no reset was accepted. Keep the
          // live lease so normal cleanup still detects a genuine release error.
          this.state = 'ready'
          this._startKeepalive()
        } else {
          // Timeout, cancellation or link loss after submission is uncertain.
          // An ownership rejection also requires dropping the obsolete lease.
          // Preserve the original failure and never replay a destructive reset.
          if (!rejected) error.reset = { acknowledged: false, outcome: 'unknown' }
          this.close(error)
        }
        throw error
      }
    })()
    const tracked = operation.finally(() => {
      if (this.resetPromise === tracked) this.resetPromise = null
    })
    this.resetPromise = tracked
    return tracked
  }

  async _softReset(command, { timeout, signal }) {
    if (!Number.isFinite(timeout) || timeout <= 0) {
      throw new RangeError('VM reset timeout must be a positive number')
    }
    const controller = new AbortController()
    const abortFromCaller = () => controller.abort(abortError(signal))
    signal?.addEventListener('abort', abortFromCaller, { once: true })
    const timeoutError = Object.assign(new Error('VM did not become ready after soft reset'), {
      code: 'VM_RESET_TIMEOUT',
    })
    const deadline = Date.now() + timeout
    const timer = setTimeout(() => controller.abort(timeoutError), timeout)
    const resetSignal = controller.signal
    const previousEpoch = this.vmEpoch
    const owner = this.sessionId
    try {
      if (signal?.aborted) abortFromCaller()
      const writeOptions = { signal: resetSignal, writeTimeout: timeout }
      await this.writeFrame('repl.signal', 'signal', null, new Uint8Array([0x03]), writeOptions)
      await this.writeFrame('repl.stdin', 'data', null, command, {
        signal: resetSignal, writeTimeout: Math.max(1, deadline - Date.now()),
      })
      this._setVmReady(false)
      while (Date.now() < deadline) {
        if (resetSignal.aborted) throw abortError(resetSignal)
        if (this.sessionId !== owner || this.state === 'disconnected') {
          throw Object.assign(new Error('Session lost while waiting for VM reset'), { code: 'SESSION_REQUIRED' })
        }
        const response = await this.rpc('session.keepalive', {}, {
          timeout: Math.min(3000, deadline - Date.now()), signal: resetSignal,
        })
        if (isVmEpochNewer(response.vmEpoch, previousEpoch)) {
          this._updateVmEpoch(response.vmEpoch)
          if (response.vmEpoch === this.vmEpoch) this._setVmReady(response.vmReady)
        }
        if (isVmEpochNewer(this.vmEpoch, previousEpoch) && this.vmReady) {
          const probeEpoch = this.vmEpoch
          if (await this.stopForRepl({ timeout: deadline - Date.now(), signal: resetSignal })
              && Date.now() < deadline && !resetSignal.aborted
              && this.sessionId === owner && this.vmEpoch === probeEpoch && this.vmReady) {
            return { ok: true, operation: 'soft-reset', acknowledged: true, vmEpoch: this.vmEpoch, vmReady: true }
          }
        }
        await awaitWithSignal(this.sleep(Math.min(100, Math.max(0, deadline - Date.now()))), resetSignal)
      }
      throw timeoutError
    } catch (error) {
      if (resetSignal.aborted) throw abortError(resetSignal)
      if (Date.now() >= deadline) throw timeoutError
      throw error
    } finally {
      clearTimeout(timer)
      signal?.removeEventListener('abort', abortFromCaller)
    }
  }

  async stopForRepl({ timeout = 5000, signal = null } = {}) {
    const deadline = Date.now() + timeout
    for (let attempt = 0; attempt < 2 && Date.now() < deadline; attempt += 1) {
      if (signal?.aborted) throw abortError(signal)
      await this.writeFrame('repl.signal', 'signal', null, new Uint8Array([0x03]), {
        signal, writeTimeout: Math.max(1, deadline - Date.now()),
      })
      const remaining = Math.min(2500, deadline - Date.now())
      if (remaining <= 0) return false
      try {
        await awaitWithSignal(this.repl.execPy('1', { timeout: remaining, signal }), signal)
        return true
      } catch (error) {
        if (signal?.aborted) throw abortError(signal)
        // Still busy; interrupt again only within the original budget.
      }
    }
    return false
  }

  async runScript(path, content, options = {}) {
    const timeout = options.timeout === undefined ? 10000 : options.timeout
    if (timeout !== null && (!Number.isFinite(timeout) || timeout <= 0)) {
      throw new RangeError('Script timeout must be a positive number or null for an interactive run')
    }
    const timeoutError = Object.assign(new Error(`script run timeout after ${timeout}ms`), { code: 'SCRIPT_TIMEOUT' })
    const operation = createOperationDeadline(options.signal ?? null, timeout, timeoutError)
    try {
      return await this._runScript(path, content, {
        ...options,
        timeout: null,
        signal: operation.signal,
      })
    } finally {
      operation.dispose()
    }
  }

  async _runScript(path, content, {
    timeout = 10000,
    signal = null,
    onStarted = null,
    maxOutputChars = Number.POSITIVE_INFINITY,
    trial = null,
    inlineCompletion = false,
    inlineSource = false,
  } = {}) {
    if (signal?.aborted) throw abortError(signal)
    if (inlineSource) {
      if (path !== null || typeof content !== 'string' || inlineCompletion || trial) {
        throw new TypeError('Inline Python source must run without a device path or typed trial')
      }
    } else {
      validateRpcPath(path)
    }
    if (inlineCompletion && trial) throw new Error('Typed trial execution cannot use an inline runner')
    if (timeout !== null && (!Number.isFinite(timeout) || timeout <= 0)) {
      throw new RangeError('Script timeout must be a positive number or null for an interactive run')
    }
    if (!inlineSource && ((content !== undefined && !this.hasFeature('file.write.binary')) || !this.hasFeature('script.run'))) {
      throw new Error('Running scripts requires script.run and uploading source also requires file.write.binary')
    }
    if (this.runCapture || this.runStarting) {
      throw new Error('A script is already running')
    }
    if (trial !== null && (!trial || typeof trial !== 'object'
        || !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(trial.operation_id ?? '')
        || typeof trial.source_path !== 'string' || utf8Bytes(trial.source_path).length > 512
        || !/^[a-f0-9]{64}$/.test(trial.source_hash ?? ''))) {
      throw new Error('Trial runner identity is invalid')
    }
    if (trial !== null && (!this.hasFeature('launcher.trial.status.v1') || !this.hasFeature('launcher.typed-runner.v1'))) {
      const error = new Error('Typed trial execution requires launcher.trial.status.v1 and launcher.typed-runner.v1 firmware features')
      error.code = 'UNSUPPORTED_OPERATION'
      throw error
    }
    this.runStarting = true
    let inlineSourceVariable = null
    let inlineSourcePrepared = false
    try {
      if (!await this.stopForRepl({ timeout: timeout === null ? 5000 : Math.min(timeout, 5000), signal })) {
        throw new Error('could not stop the running script (no REPL answer after Ctrl-C)')
      }
      if (inlineSource) {
        inlineSourceVariable = `__evcore_python_${Date.now().toString(36)}_${++this.sequence}`
        const assignments = this.repl.createInlineSourceAssignments(content, inlineSourceVariable)
        for (const assignment of assignments) {
          inlineSourcePrepared = true
          await this.repl.execPy(assignment, { timeout: INLINE_PYTHON_REPL_TIMEOUT_MS, signal })
        }
      } else if (content !== undefined) {
        await this.writeScript(path, content, { signal })
      }
    } catch (error) {
      if (inlineSourcePrepared) {
        await this.repl.execPy(`del ${inlineSourceVariable}`, { timeout: 3000 }).catch(() => {})
      }
      throw error
    } finally {
      this.runStarting = false
    }
    const completion = new Promise((resolve, reject) => {
      const timer = timeout === null ? null : setTimeout(() => {
        const capture = this.runCapture
        this.runCapture = null
        cleanup()
        // Keep the sentinel registered: if the best-effort interrupt lands,
        // its queued marker is still consumed instead of leaking to output.
        this.interrupt().catch(() => {})
        const error = new Error(`script run timeout after ${timeout}ms${capture?.output ? `\n${capture.output}` : ''}`)
        error.code = 'SCRIPT_TIMEOUT'
        reject(error)
      }, timeout)
      const onAbort = () => {
        const error = abortError(signal)
        this.runCapture = null
        clearTimeout(timer)
        cleanup()
        this.interrupt().catch(() => {})
        reject(error)
      }
      signal?.addEventListener('abort', onAbort, { once: true })
      const cleanup = () => signal?.removeEventListener('abort', onAbort)
      this.runCapture = {
        path,
        output: '',
        outputTruncated: false,
        maxOutputChars,
        resolve: (value) => {
          cleanup()
          resolve(value)
        },
        reject: (error) => {
          cleanup()
          reject(error)
        },
        timer,
      }
    })
    // Completion may reject during a startup RPC or stdin write. Observe it
    // immediately; the await below still returns the original error.
    completion.catch(() => {})

    try {
      if (inlineSource) {
        const command = this.repl.createInlineScriptRunCommand(inlineSourceVariable)
        await this.repl.writeStdin(command, { signal })
      } else if (inlineCompletion) {
        const command = this.repl.createScriptRunCommand(path, { acknowledgeStart: Boolean(onStarted) })
        if (this.repl.scriptRunPrelude) await this.repl.execPy(this.repl.scriptRunPrelude, { timeout: timeout === null ? 5000 : timeout, signal })
        const startAcknowledgement = onStarted
          ? this.repl.waitForScriptStart({ timeout: SCRIPT_START_ACK_TIMEOUT_MS, signal })
          : null
        startAcknowledgement?.catch(() => {})
        await this.repl.writeStdin(command, { signal })
        if (startAcknowledgement) await startAcknowledgement
      } else {
        const sentinelLine = this.repl.createScriptEndSentinel()
        const request = { path }
        if (trial) {
          request.trial_operation_id = trial.operation_id
          request.trial_source_path = trial.source_path
          request.trial_source_hash = trial.source_hash
        }
        await this.rpc('script.run', request, { timeout: 4000, signal })
        await this.repl.writeStdin(sentinelLine, { signal })
      }
      await onStarted?.({ path, trial: trial ? structuredClone(trial) : null })
      return await completion
    } catch (err) {
      const capture = this.runCapture
      this.runCapture = null
      clearTimeout(capture?.timer)
      this.repl.cancelScriptEndSentinel(err)
      capture?.reject(err)
      try {
        await completion
      } catch (_err) {
        // Return the initiating failure below.
      }
      throw err
    }
  }

  async startScript(path, content, { onStarted = null, ...options } = {}) {
    let resolveStarted
    let rejectStarted
    let didStart = false
    const started = new Promise((resolve, reject) => {
      resolveStarted = resolve
      rejectStarted = reject
    })
    const done = this.runScript(path, content, {
      timeout: null,
      maxOutputChars: 0,
      ...options,
      onStarted: async (info) => {
        await onStarted?.(info)
        didStart = true
        resolveStarted(info)
      },
    })
    done.catch((error) => {
      if (!didStart) rejectStarted(error)
    })
    return { ...await started, done }
  }

  scriptRunSnapshot() {
    if (!this.runCapture) return null
    return {
      path: this.runCapture.path,
      output: this.runCapture.output.replace(/\r/g, ''),
      outputTruncated: this.runCapture.outputTruncated,
    }
  }

  snapshot() {
    return {
      state: this.state,
      role: this.role,
      capabilities: this.capabilities,
      parser: this.parser.getDiagnostics?.(),
      transport: this.transport.getDiagnostics?.(),
      pendingRpc: this.pendingRpc.size,
      scriptRunning: Boolean(this.runCapture),
    }
  }

  close(error = new Error('connection closed')) {
    this.negotiateController?.abort(error)
    this.draining = false
    clearTimeout(this.keepaliveTimer)
    this.keepaliveTimer = null
    this.keepaliveGeneration += 1
    this.sessionGeneration += 1
    this.state = 'disconnected'
    this.capabilities = null
    this.sessionId = null
    this.vmEpoch = null
    this.vmReady = false
    this.vmReadyKnown = false
    this.vmReadinessProbeActive = false
    this.parser.active = false
    for (const pending of this.pendingRpc.values()) {
      pending.reject(error)
    }
    this.pendingRpc.clear()
    if (this.runCapture) {
      clearTimeout(this.runCapture.timer)
      this.runCapture.reject(error)
      this.runCapture = null
    }
    this.repl.failAll(error)
  }
}

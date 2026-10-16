/*
 * SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */


export const EPHEMERAL_PYTHON_PATH = '/.espvision_agent_run.py'

const MAX_EVIDENCE_ENTRIES = 500
const MAX_RUN_OUTPUT_CHARS = 64 * 1024
const MAX_EVIDENCE_VALUE_CHARS = 8 * 1024

function boundedEvidenceValue(value) {
  if (typeof value === 'string') {
    return value.length <= MAX_EVIDENCE_VALUE_CHARS
      ? value
      : `${value.slice(0, MAX_EVIDENCE_VALUE_CHARS)}…[truncated]`
  }
  try {
    const serialized = JSON.stringify(value)
    return serialized.length <= MAX_EVIDENCE_VALUE_CHARS
      ? value
      : { truncated: true, preview: serialized.slice(0, MAX_EVIDENCE_VALUE_CHARS) }
  } catch (_error) {
    return String(value).slice(0, MAX_EVIDENCE_VALUE_CHARS)
  }
}

function randomId() {
  return globalThis.crypto?.randomUUID?.()
    || `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`
}

function abortError(message = 'Temporary Python run cancelled') {
  const error = new Error(message)
  error.name = 'AbortError'
  return error
}

function delay(ms, signal = null) {
  if (ms <= 0) return Promise.resolve()
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      cleanup()
      resolve()
    }, ms)
    const onAbort = () => {
      clearTimeout(timer)
      cleanup()
      reject(abortError())
    }
    const cleanup = () => signal?.removeEventListener('abort', onAbort)
    signal?.addEventListener('abort', onAbort, { once: true })
  })
}

function validateDevicePath(path) {
  if (typeof path !== 'string' || !path.startsWith('/') || path.includes('\0')) {
    throw new Error('Device path must be an absolute path without NUL bytes')
  }
  return path
}

function base64ToBytes(value) {
  const binary = globalThis.atob(value)
  return Uint8Array.from(binary, char => char.charCodeAt(0))
}

function bytesToBase64(bytes) {
  let binary = ''
  for (const byte of bytes) binary += String.fromCharCode(byte)
  return globalThis.btoa(binary)
}

export class DeviceEvidenceBuffer {
  constructor(limit = MAX_EVIDENCE_ENTRIES) {
    this.limit = limit
    this.entries = []
  }

  append(kind, value, details = null) {
    this.entries.push({
      time: new Date().toISOString(),
      kind,
      value: boundedEvidenceValue(value),
      ...(details == null ? {} : { details }),
    })
    if (this.entries.length > this.limit) {
      this.entries.splice(0, this.entries.length - this.limit)
    }
  }

  snapshot(limit = 100) {
    return this.entries.slice(-Math.max(0, Math.min(this.limit, limit)))
  }

  clear() {
    this.entries.length = 0
  }
}

export class EphemeralPythonController {
  constructor(session, { evidence = null, generation = 'session' } = {}) {
    this.session = session
    this.evidence = evidence
    this.generation = generation
    this.active = null
    this.last = null
  }

  _snapshot(run, outputOffset = 0) {
    const liveOutput = run === this.active
      ? this.session.scriptRunSnapshot()?.output ?? run.output
      : run.output
    const output = String(liveOutput || '').slice(0, MAX_RUN_OUTPUT_CHARS)
    const offset = Math.min(output.length, outputOffset)
    return {
      runId: run.id,
      path: EPHEMERAL_PYTHON_PATH,
      status: run.status,
      output: output.slice(offset),
      nextOffset: output.length,
      finished: !['starting', 'running', 'stopping'].includes(run.status),
      cleaned: run.cleaned,
      ...(run.error ? { error: run.error } : {}),
      ...(run.outputTruncated ? { outputTruncated: true } : {}),
      ...(run.cleanupError ? {
        cleanupError: run.cleanupError,
        residualPath: EPHEMERAL_PYTHON_PATH,
      } : {}),
    }
  }

  _find(runId) {
    const run = this.active?.id === runId ? this.active : this.last?.id === runId ? this.last : null
    if (!run) throw new Error(`Unknown or stale temporary run: ${runId}`)
    return run
  }

  async start(code, { maxDurationMs = 180000, signal = null } = {}) {
    if (signal?.aborted) throw abortError()
    if (this.active && ['starting', 'running', 'stopping'].includes(this.active.status)) {
      throw new Error('A temporary Python run is already active')
    }
    const controller = new AbortController()
    const run = {
      id: `${this.generation}:${randomId()}`,
      status: 'starting',
      output: '',
      cleaned: false,
      controller,
      requestedStop: false,
      done: null,
    }
    this.active = run
    let resolveStarted
    let rejectStarted
    const started = new Promise((resolve, reject) => {
      resolveStarted = resolve
      rejectStarted = reject
    })
    const onStartAbort = () => controller.abort(signal?.reason || abortError())
    signal?.addEventListener('abort', onStartAbort, { once: true })
    run.done = this.session.runScript(EPHEMERAL_PYTHON_PATH, code, {
      timeout: maxDurationMs,
      signal: controller.signal,
      onStarted: () => {
        run.status = 'running'
        this.evidence?.append('python.ephemeral.started', { runId: run.id })
        resolveStarted()
      },
      maxOutputChars: MAX_RUN_OUTPUT_CHARS,
    }).then(result => {
      run.status = 'finished'
      run.output = result.output || ''
      run.outputTruncated = result.outputTruncated === true
    }).catch(error => {
      const failedBeforeStart = run.status === 'starting'
      const snapshot = this.session.scriptRunSnapshot()
      run.output = snapshot?.output || run.output
      run.outputTruncated = snapshot?.outputTruncated === true
      if (run.requestedStop || error?.name === 'AbortError') run.status = 'stopped'
      else if (/timeout/i.test(error?.message || '')) run.status = 'timed-out'
      else run.status = 'failed'
      run.error = error?.message || String(error)
      if (failedBeforeStart) rejectStarted(error)
    }).finally(async () => {
      signal?.removeEventListener('abort', onStartAbort)
      try {
        await this.session.stopForRepl()
        await this.session.repl.removeFile(EPHEMERAL_PYTHON_PATH)
        run.cleaned = true
      } catch (error) {
        run.cleaned = false
        run.cleanupError = error?.message || String(error)
      }
      this.evidence?.append('python.ephemeral.finished', this._snapshot(run))
      this.last = run
      if (this.active === run) this.active = null
    })
    await started
    return this._snapshot(run)
  }

  async observe(runId, { waitMs = 1000, outputOffset = 0, signal = null } = {}) {
    const run = this._find(runId)
    if (!this._snapshot(run).finished && waitMs > 0) {
      await Promise.race([run.done, delay(waitMs, signal)])
    }
    return this._snapshot(run, outputOffset)
  }

  async stop(runId) {
    const run = this._find(runId)
    if (!this._snapshot(run).finished) {
      run.status = 'stopping'
      run.requestedStop = true
      run.controller.abort(abortError('Temporary Python run stopped'))
      await run.done
    }
    return this._snapshot(run)
  }

  async close() {
    if (!this.active) return null
    return this.stop(this.active.id)
  }
}

export function bindSessionDeviceActions(registry, {
  evidence = new DeviceEvidenceBuffer(),
  lease = null,
  getOperations = () => null,
  getGeneration = () => 'session',
} = {}) {
  const controllers = new WeakMap()
  const controllerFor = session => {
    let controller = controllers.get(session)
    if (!controller) {
      controller = new EphemeralPythonController(session, {
        evidence,
        generation: getGeneration(),
      })
      controllers.set(session, controller)
    }
    return controller
  }

  registry.bind('device.negotiate', (input, { session, signal }) => (
    session.negotiate({ role: input.role, drainMs: input.drainMs, signal })
  ))
  registry.bind('device.inspect', async (input, { session, signal }) => {
    const sessionState = session.snapshot()
    if (sessionState.scriptRunning) {
      return {
        session: sessionState,
        ...(lease ? { lease: lease.snapshot() } : {}),
        ...(getOperations() ? { operations: getOperations() } : {}),
        inspectionLimited: 'A temporary Python run is active; REPL filesystem inspection is deferred until it stops.',
        recentLogs: evidence.snapshot(input.logLimit),
      }
    }
    const [deviceInfo, fsStats, files] = await Promise.all([
      session.repl.getDeviceInfo({ signal }),
      session.repl.getFsStats('/', { signal }),
      input.includeFiles ? session.repl.walkFs({ signal }) : Promise.resolve(undefined),
    ])
    return {
      session: sessionState,
      ...(lease ? { lease: lease.snapshot() } : {}),
      ...(getOperations() ? { operations: getOperations() } : {}),
      deviceInfo,
      fsStats,
      ...(files ? { files } : {}),
      recentLogs: evidence.snapshot(input.logLimit),
    }
  })
  registry.bind('python.execute', async (input, { session, signal }) => ({
    ok: true,
    output: (await session.executePython(input.code, { timeout: input.timeoutMs, signal })).replace(/\r/g, ''),
  }))
  registry.bind('python.runEphemeral', (input, { session, signal }) => (
    controllerFor(session).start(input.code, { maxDurationMs: input.maxDurationMs, signal })
  ))
  registry.bind('python.observeEphemeral', (input, { session, signal }) => (
    controllerFor(session).observe(input.runId, {
      waitMs: input.waitMs,
      outputOffset: input.outputOffset,
      signal,
    })
  ))
  registry.bind('python.stopEphemeral', (input, { session }) => (
    controllerFor(session).stop(input.runId)
  ))
  registry.bind('deviceFile.read', async (input, { session, signal }) => {
    const path = validateDevicePath(input.path)
    const bytes = await session.repl.readFile(path, { signal })
    return {
      ok: true,
      path,
      encoding: input.encoding,
      content: input.encoding === 'base64' ? bytesToBase64(bytes) : new TextDecoder().decode(bytes),
    }
  })
  registry.bind('deviceFile.write', async (input, { session, signal }) => {
    const path = validateDevicePath(input.path)
    const bytes = input.encoding === 'base64' ? base64ToBytes(input.content) : null
    await session.writeFile(path, bytes ?? input.content, {
      encoding: bytes ? 'binary' : 'utf-8', runAfterWrite: input.runAfterWrite, signal,
    })
    return { ok: true, path, bytes: bytes?.length ?? new TextEncoder().encode(input.content).length }
  })
  registry.bind('deviceFile.remove', async (input, { session, signal }) => {
    const path = validateDevicePath(input.path)
    if (input.kind === 'directory') await session.repl.removeDir(path, { signal })
    else await session.repl.removeFile(path, { signal })
    return { ok: true, path, kind: input.kind }
  })
  registry.bind('deviceTest.run', (input, { session, signal }) => (
    session.runScript(validateDevicePath(input.path), input.source, { timeout: input.timeoutMs, signal })
  ))
  registry.bind('device.control', (input, { session, signal }) => session.control(input.operation, { signal }))
  registry.bind('device.rpc', (input, { session, signal }) => session.rpc(
    input.method,
    input.payload,
    { timeout: input.timeoutMs, channel: input.channel, signal },
  ))
  registry.bind('frame.capture', async (input, { session, signal }) => {
    session.lastDebugImage = null
    await session.rpc('debug.capture_frame', {}, {
      timeout: input.timeoutMs,
      channel: 'debug.rpc',
      signal,
    })
    if (!session.lastDebugImage) throw new Error('Firmware returned no JPEG frame')
    return session.lastDebugImage
  })

  return { evidence, controllerFor }
}

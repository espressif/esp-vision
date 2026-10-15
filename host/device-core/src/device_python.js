// SPDX-License-Identifier: Apache-2.0

export function checkAbort(signal) {
  if (signal !== undefined && signal !== null && !(signal instanceof AbortSignal)) {
    throw new TypeError('signal must be an AbortSignal')
  }
  if (signal?.aborted) {
    throw signal.reason instanceof Error ? signal.reason : Object.assign(new Error('Operation aborted'), { name: 'AbortError' })
  }
}

export function snapshotRequest(request, fields, label = 'operation') {
  if (!request || typeof request !== 'object' || Array.isArray(request)
      || ![Object.prototype, null].includes(Object.getPrototypeOf(request))) {
    throw new TypeError(`Expected a ${label} request object`)
  }
  for (const key of Reflect.ownKeys(request)) {
    if (!fields.includes(key)) throw new TypeError(`Unknown ${label} parameter: ${String(key)}`)
  }
  const snapshot = { ...request }
  checkAbort(snapshot.signal)
  return snapshot
}

export function validateDevicePath(path, { root = false } = {}) {
  if (typeof path !== 'string' || !path.startsWith('/') || (!root && path === '/')
      || new TextEncoder().encode(path).length > 123
      || /[\x00-\x1f\x7f\\"]/.test(path) || path.includes('..') || path.includes('//')
      || (path.length > 1 && path.endsWith('/'))) {
    throw Object.assign(new TypeError('Expected an absolute device path within the firmware path limit'), { code: 'INVALID_PATH' })
  }
  return path
}

export function deviceBusy() {
  return Object.assign(new Error('Device Python interpreter is busy'), { code: 'DEVICE_BUSY' })
}

export async function withDeviceOperation(state, kind, operation) {
  if (state.operation || state.run) throw deviceBusy()
  const token = { kind, interactive: false }
  state.operation = token
  try { return await operation(token) }
  finally { if (state.operation === token) state.operation = null }
}

function validateRequest(request) {
  if (!request || typeof request !== 'object' || Array.isArray(request)) throw new TypeError('Expected a Python execution request')
  const source = Object.hasOwn(request, 'source')
  const path = Object.hasOwn(request, 'path')
  if (source === path) throw new TypeError('Specify exactly one of source or path')
  if (source && typeof request.source !== 'string') throw new TypeError('Python source must be a string')
  if (path) validateDevicePath(request.path)
  const completion = request.completion ?? 'wait'
  if (!['wait', 'start'].includes(completion)) throw new TypeError('Python completion must be wait or start')
  if (request.timeout !== undefined && (!Number.isFinite(request.timeout) || request.timeout <= 0)) throw new RangeError('Python timeout must be positive')
  if (request.maxOutputChars !== undefined && (!Number.isSafeInteger(request.maxOutputChars) || request.maxOutputChars < 0)) throw new RangeError('maxOutputChars must be a nonnegative integer')
  if (completion === 'start' && request.timeout !== undefined) throw new TypeError('timeout only applies to completion wait')
  return { source, completion }
}

export function createPythonApi({ session, state = {} }) {
  let sequence = 0
  const normalize = (result, path, limit) => {
    const output = (typeof result === 'string' ? result : result?.output ?? '').replace(/\r/g, '')
    return {
      ok: result?.ok ?? true, path,
      output: limit === undefined ? output : output.slice(0, limit),
      outputTruncated: Boolean(result?.outputTruncated || (limit !== undefined && output.length > limit)),
    }
  }
  return Object.freeze({
    async input(data) {
      if (typeof data !== 'string' && !(data instanceof Uint8Array)) throw new TypeError('Python input must be a string or Uint8Array')
      if (data instanceof Uint8Array) data = new Uint8Array(data)
      if (state.operation?.interactive) return session.repl.writeStdin(data)
      return withDeviceOperation(state, 'python.input', () => session.repl.writeStdin(data))
    },
    async execute(request) {
      request = snapshotRequest(request, ['source', 'path', 'completion', 'timeout', 'signal', 'maxOutputChars'], 'Python execution')
      const { source, completion } = validateRequest(request)
      const { signal = null, timeout = 10000, maxOutputChars } = request
      checkAbort(signal)
      if (state.operation || state.run || session.runCapture || session.runStarting) throw deviceBusy()
      const token = { kind: 'python.execute', interactive: false }
      state.operation = token
      const temporary = source && completion === 'start'
      const path = source ? (temporary ? `/.__evcore_${Date.now().toString(36)}_${++sequence}.py` : null) : request.path
      const run = { path, completion, status: 'starting' }
      state.run = run
      let released = false
      const release = async (failed = false) => {
        if (released) return
        released = true
        try {
          if (temporary) {
            if (failed && session.stopForRepl) {
              const ready = await session.stopForRepl({ timeout: 3000 })
              if (!ready) throw Object.assign(new Error('Cannot clean up Python source until REPL is ready'), { code: 'REPL_UNAVAILABLE' })
            }
            await session.repl.removeFile(path)
          }
        } finally {
          if (state.operation === token) state.operation = null
          if (state.run === run) state.run = null
        }
      }
      try {
        if (completion === 'wait') {
          run.status = 'running'
          const result = source
            ? await session.executePython(request.source, { timeout, signal })
            : await session.runScript(path, undefined, { timeout, signal, maxOutputChars, inlineCompletion: true })
          await release()
          return normalize(result, path, maxOutputChars)
        }
        const started = await session.startScript(path, source ? request.source : undefined, { signal, maxOutputChars, inlineCompletion: true })
        run.status = 'running'
        token.interactive = true
        const done = started.done.then(result => normalize(result, path, maxOutputChars)).then(async result => {
          await release()
          return result
        }, async error => {
          try { await release(true) } catch (_cleanupError) { /* Preserve the execution error. */ }
          throw error
        })
        // A caller may only need the started acknowledgement and consume errors via events.
        done.catch(() => {})
        return { ok: true, path, done }
      } catch (error) {
        try { await release(true) } catch (_cleanupError) { /* Preserve the execution error. */ }
        throw error
      }
    },
  })
}

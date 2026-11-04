// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { Socket } from 'node:net'
import { spawn } from 'node:child_process'
import { fileURLToPath } from 'node:url'
const fail = (code, message) => Object.assign(new Error(message), { code })

function subscribers() {
  let listener = null
  return {
    subscribe(value) { if (listener) throw fail('TRANSPORT_BUSY', 'Already subscribed'); listener = value; return () => { listener = null } },
    emit(event, value) { listener?.[event]?.(value) },
  }
}

export function tcpTransport(options) {
  const events = subscribers()
  let socket
  return {
    subscribe: events.subscribe,
    connect({ signal }) {
      return new Promise((resolveConnect, reject) => {
        socket = new Socket()
        socket.setNoDelay(true)
        const abort = () => { socket.destroy(); reject(fail('CANCELLED', 'Connection cancelled')) }
        const cleanup = () => signal.removeEventListener('abort', abort)
        socket.on('data', bytes => events.emit('data', bytes))
        socket.on('error', () => { const error = fail('TCP_ERROR', 'TCP transport failed'); cleanup(); reject(error); events.emit('error', error) })
        socket.on('close', () => { cleanup(); events.emit('close'); reject(fail('TCP_CLOSED', 'TCP connection closed')) })
        signal.addEventListener('abort', abort, { once: true })
        if (signal.aborted) return abort()
        socket.connect(options.port, options.host, () => { cleanup(); resolveConnect() })
      })
    },
    writeBytes(bytes) { return new Promise((resolveWrite, reject) => socket?.writable ? socket.write(bytes, error => error ? reject(fail('TCP_WRITE', 'TCP write failed')) : resolveWrite()) : reject(fail('TCP_CLOSED', 'TCP connection is closed'))) },
    disconnect() { socket?.destroy() },
  }
}

export function serialTransport(options) {
  const events = subscribers()
  let child, controlId = 0, baudRate = 115200
  const pending = new Map()
  const rejectControls = error => {
    for (const request of pending.values()) request.reject(error)
    pending.clear()
  }
  const send = (kind, bytes) => new Promise((resolveWrite, reject) => {
    if (!child?.stdin.writable) return reject(fail('SERIAL_CLOSED', 'Serial adapter closed'))
    const record = Buffer.allocUnsafe(5 + bytes.byteLength)
    record.writeUInt8(kind, 0)
    record.writeUInt32BE(bytes.byteLength, 1)
    record.set(bytes, 5)
    child.stdin.write(record, error => error ? reject(fail('SERIAL_WRITE', 'Serial write failed')) : resolveWrite())
  })
  return {
    subscribe: events.subscribe,
    connect({ signal }) {
      return new Promise((resolveConnect, reject) => {
        const adapter = fileURLToPath(new URL('./node_serial.py', import.meta.url))
        child = spawn(options.python, ['-u', adapter, options.serial], { stdio: ['pipe', 'pipe', 'pipe'] })
        let ready = false, settled = false, runtimeErrorReported = false, stderr = '', lines = ''
        const abort = () => {
          child.kill('SIGTERM')
          if (!settled) {
            settled = true
            reject(fail('CANCELLED', 'Serial connection cancelled'))
          }
        }
        const cleanup = () => signal.removeEventListener('abort', abort)
        const serialError = () => {
          const code = ['SERIAL_ACCESS_DENIED', 'SERIAL_BUSY'].find(code => stderr.includes(code)) ?? 'SERIAL_ERROR'
          return fail(code, 'Serial adapter failed; ensure Python, pyserial and port access are available')
        }
        const failBeforeReady = () => {
          if (settled) return
          settled = true
          cleanup()
          const error = serialError()
          reject(error)
          events.emit('error', error)
        }
        const reportRuntimeError = () => {
          if (runtimeErrorReported) return
          runtimeErrorReported = true
          events.emit('error', serialError())
        }
        child.on('error', () => { if (ready) reportRuntimeError() })
        child.stdin.on('error', () => { if (ready) reportRuntimeError() })
        child.stdout.on('data', bytes => events.emit('data', bytes))
        child.stderr.on('data', bytes => {
          // Do not print Python errors: they can contain host paths or device data.
          stderr = (stderr + bytes.toString()).slice(-128)
          lines = (lines + bytes.toString()).slice(-8192)
          for (let newline; (newline = lines.indexOf('\n')) !== -1;) {
            const line = lines.slice(0, newline).trim()
            lines = lines.slice(newline + 1)
            if (line === 'READY' && !ready) {
              ready = true
              settled = true
              baudRate = 115200
              cleanup()
              resolveConnect()
              continue
            }
            let response
            try { response = JSON.parse(line) } catch { continue }
            const request = pending.get(response.id)
            if (!request) continue
            pending.delete(response.id)
            if (request.kind === 'drain') {
              if (response.ok === true && response.drained === true) request.resolve()
              else request.reject(fail('SERIAL_DRAIN_FAILED', 'Serial adapter could not drain pending output'))
            } else if (response.ok && response.baudRate === request.baudRate) {
              baudRate = response.baudRate
              request.resolve()
            } else request.reject(fail('SERIAL_BAUD_UNSUPPORTED', 'Serial adapter rejected the requested baudrate'))
          }
        })
        // Wait for close rather than exit: stdout/stderr may still have buffered
        // data after exit, including the adapter's stable SERIAL_BUSY marker.
        child.on('close', () => { cleanup(); rejectControls(fail('SERIAL_CLOSED', 'Serial adapter closed')); if (!ready) failBeforeReady(); events.emit('close') })
        signal.addEventListener('abort', abort, { once: true })
        if (signal.aborted) abort()
      })
    },
    writeBytes(bytes) { return send(0, bytes) },
    drain({ signal } = {}) {
      if (signal?.aborted) return Promise.reject(signal.reason ?? fail('CANCELLED', 'Serial drain cancelled'))
      const id = ++controlId
      return new Promise((resolve, reject) => {
        const cleanup = () => { clearTimeout(timer); signal?.removeEventListener('abort', abort); pending.delete(id) }
        const abort = () => { cleanup(); reject(signal.reason ?? fail('CANCELLED', 'Serial drain cancelled')) }
        const timer = setTimeout(() => { cleanup(); reject(fail('SERIAL_DRAIN_TIMEOUT', 'Serial drain timed out')) }, 2000)
        pending.set(id, {
          kind: 'drain',
          resolve() { cleanup(); resolve() },
          reject(error) { cleanup(); reject(error) },
        })
        signal?.addEventListener('abort', abort, { once: true })
        send(2, Buffer.from(JSON.stringify({ id }))).catch(error => pending.get(id)?.reject(error))
      })
    },
    setBaudRate(speed, { signal } = {}) {
      if (!Number.isSafeInteger(speed) || speed <= 0) return Promise.reject(new RangeError('baudRate must be a positive integer'))
      if (signal?.aborted) return Promise.reject(signal.reason ?? fail('CANCELLED', 'Serial speed change cancelled'))
      const id = ++controlId
      return new Promise((resolve, reject) => {
        const cleanup = () => { clearTimeout(timer); signal?.removeEventListener('abort', abort); pending.delete(id) }
        const abort = () => { cleanup(); reject(signal.reason ?? fail('CANCELLED', 'Serial speed change cancelled')) }
        const timer = setTimeout(() => { cleanup(); reject(fail('SERIAL_BAUD_TIMEOUT', 'Serial speed change timed out')) }, 2000)
        pending.set(id, {
          baudRate: speed,
          resolve() { cleanup(); resolve() },
          reject(error) { cleanup(); reject(error) },
        })
        signal?.addEventListener('abort', abort, { once: true })
        send(1, Buffer.from(JSON.stringify({ id, baudRate: speed }))).catch(error => pending.get(id)?.reject(error))
      })
    },
    getDiagnostics() { return { baudRate } },
    async disconnect() {
      rejectControls(fail('SERIAL_CLOSED', 'Serial adapter closed'))
      if (!child?.pid || child.exitCode !== null || child.signalCode !== null) return
      await new Promise(resolveExit => {
        const timer = setTimeout(() => { child.kill('SIGKILL') }, 2000)
        child.once('exit', () => { clearTimeout(timer); resolveExit() })
        child.stdin.end()
        child.kill('SIGTERM')
      })
    },
  }
}

// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { Socket } from 'node:net'
import { NodeSerialPort, boundedSerial, serialFailure } from './node_serial.js'
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

export function serialTransport(options, dependencies = {}) {
  const events = subscribers()
  let port, reader, reading, connected = false, opening = false, generation = 0, lastError = null, baudRate = 115200
  let queue = Promise.resolve(), closing
  const ordered = operation => {
    const active = generation, current = port
    const result = queue.then(async () => {
      if (active !== generation) throw fail('SERIAL_CLOSED', 'Serial connection changed before the operation')
      const value = await operation(current, active)
      if (active !== generation) throw fail('SERIAL_CLOSED', 'Serial connection closed during the operation')
      return value
    })
    queue = result.catch(() => {})
    return result
  }
  const assertConnected = () => { if (!connected || !port?.handle) throw fail('SERIAL_CLOSED', 'Serial port is closed') }
  const report = (error, stage) => {
    const failure = error.code?.startsWith('SERIAL_') ? error : serialFailure(error, stage)
    lastError = failure.details ?? { stage, code: failure.code, type: failure.name }
    return failure
  }
  const adapter = {
    subscribe: events.subscribe,
    async connect({ signal } = {}) {
      if (connected || opening || closing) throw fail('TRANSPORT_BUSY', 'Serial transport is already connected or closing')
      signal?.throwIfAborted()
      opening = true
      const active = ++generation
      const current = port = new NodeSerialPort({ path: options.serial }, { ...dependencies, signals: { dtr: true, rts: false } })
      const abort = () => { ++generation; connected = false; void current.close().catch(() => {}) }
      signal?.addEventListener('abort', abort, { once: true })
      try {
        await boundedSerial(() => current.open(), { signal, timeout: 10000 })
        signal?.throwIfAborted()
        if (active !== generation) throw fail('SERIAL_CLOSED', 'Serial connection cancelled')
        connected = true; baudRate = 115200; lastError = null
        const input = reader = current.readable.getReader()
        reading = (async () => {
          try {
            while (active === generation && connected) {
              const { value, done } = await input.read()
              if (active !== generation) break
              if (done) throw fail('SERIAL_CLOSED', 'Serial device disconnected')
              if (value?.length) events.emit('data', value)
            }
          } catch (error) {
            if (active === generation) {
              connected = false
              events.emit('error', report(error, 'read'))
              events.emit('close')
              await current.close().catch(() => {})
            }
          } finally { input.releaseLock(); if (reader === input) reader = null }
        })()
      } catch (error) { await current.close().catch(() => {}); throw report(error, 'open') }
      finally { opening = false; signal?.removeEventListener('abort', abort) }
    },
    writeBytes(bytes) {
      // Snapshot now: callers may reuse DMA/frame buffers once this resolves.
      const data = Buffer.from(bytes)
      return ordered(async (current, active) => {
        assertConnected()
        try { await boundedSerial(() => current.handle.write(data), { timeout: 2000 }) }
        catch (error) { if (active !== generation) throw fail('SERIAL_CLOSED', 'Serial connection closed during write'); const failure = report(error, 'write'); await adapter.disconnect(); throw failure }
      })
    },
    drain({ signal } = {}) {
      return ordered(async (current, active) => {
        assertConnected()
        try { await boundedSerial(() => current.handle.drain(), { signal, timeout: 2000 }) }
        catch (error) { if (active !== generation) throw fail('SERIAL_CLOSED', 'Serial connection closed during drain'); await adapter.disconnect(); throw report(error, 'drain') }
      })
    },
    setBaudRate(speed, { signal } = {}) {
      if (!Number.isSafeInteger(speed) || speed <= 0) return Promise.reject(new RangeError('baudRate must be a positive integer'))
      return ordered(async (current, active) => {
        assertConnected()
        try {
          await boundedSerial(() => current.setBaudRate(speed), { signal, timeout: 2000 })
          if (active === generation) baudRate = speed
        } catch (error) { if (active !== generation) throw fail('SERIAL_CLOSED', 'Serial connection closed during baud change'); await adapter.disconnect(); throw report(error, 'baud') }
      })
    },
    getDiagnostics() { return { baudRate, connected, ...(lastError ? { lastError: { ...lastError } } : {}) } },
    disconnect() {
      if (closing) return closing
      const wasConnected = connected
      const current = port, input = reader, pendingRead = reading
      ++generation; connected = false
      // Closing the native handle interrupts pending reads/writes; never wait
      // behind a stalled operation in the ordered write queue.
      closing = (async () => {
        await input?.cancel().catch(() => {})
        await current?.close()
        await pendingRead
        if (wasConnected) events.emit('close')
      })().finally(() => { closing = null })
      return closing
    },
  }
  return adapter
}

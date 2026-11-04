// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

const failure = (code, message) => Object.assign(new Error(message), { code })
const cancelled = signal => signal?.reason instanceof Error ? signal.reason
  : Object.assign(new Error('Serial operation cancelled'), { name: 'AbortError' })

/** Passive Web Serial byte adapter. The UI supplies its already authorized port. */
export function serialTransport({ port, signals = { dataTerminalReady: true, requestToSend: false }, bufferSize = 256 * 1024 } = {}) {
  if (!port || typeof port.open !== 'function' || typeof port.close !== 'function') throw new TypeError('A browser SerialPort is required')
  if (!Number.isSafeInteger(bufferSize) || bufferSize <= 0 || bufferSize > 16 * 1024 * 1024) throw new RangeError('bufferSize must be between 1 byte and 16 MiB')
  let listener = null, reader = null, writer = null, reading = null
  let stopped = true, reopening = false, opened = false, opening = false, generation = 0, baudRate = 115200
  let queue = Promise.resolve()
  const ordered = operation => {
    const result = queue.then(operation)
    queue = result.catch(() => {})
    return result
  }
  const emit = (event, value) => listener?.[event]?.(value)
  const check = signal => {
    if (signal?.aborted) throw cancelled(signal)
    if (stopped) throw failure('SERIAL_CLOSED', 'Browser serial adapter is closed')
  }
  const disconnected = () => { if (!stopped) emit('close') }
  const stopStreams = async () => {
    generation += 1
    try { await reader?.cancel() } catch { /* Unplugged streams may already be errored. */ }
    await reading?.catch(() => {})
    reading = null
    reader = null
    if (writer) { writer.releaseLock(); writer = null }
  }
  const closePort = async () => {
    await stopStreams()
    if (opened) {
      await port.close()
      opened = false
    }
  }
  const startStreams = () => {
    if (!port.readable || !port.writable) throw failure('SERIAL_CLOSED', 'Browser serial streams are unavailable')
    const active = ++generation
    const input = reader = port.readable.getReader()
    writer = port.writable.getWriter()
    reading = (async () => {
      try {
        while (!stopped && active === generation) {
          const { value, done } = await input.read()
          if (done) {
            if (!stopped && !reopening && active === generation) emit('close')
            break
          }
          if (value?.byteLength && active === generation) emit('data', value)
        }
      } catch {
        if (!stopped && !reopening && active === generation) emit('error', failure('SERIAL_READ', 'Browser serial read failed'))
      } finally {
        input.releaseLock()
        if (reader === input) reader = null
      }
    })()
  }
  const open = async (speed, signal) => {
    check(signal)
    opening = true
    try {
      await port.open({ baudRate: speed, bufferSize })
      opened = true
      check(signal)
      try { await port.setSignals?.(signals) } catch { /* Native USB interfaces may not expose modem control lines. */ }
      check(signal)
      baudRate = speed
      startStreams()
    } catch (error) { await closePort().catch(() => {}); throw error }
    finally { opening = false }
  }
  return {
    subscribe(value) {
      if (listener) throw failure('TRANSPORT_BUSY', 'Already subscribed')
      listener = value
      return () => { if (listener === value) listener = null }
    },
    connect({ signal } = {}) {
      if (!stopped) return Promise.reject(failure('TRANSPORT_BUSY', 'Browser serial adapter is already connected'))
      stopped = false
      port.addEventListener?.('disconnect', disconnected)
      const connecting = ordered(async () => {
        try { await open(115200, signal) }
        catch (error) {
          stopped = true
          port.removeEventListener?.('disconnect', disconnected)
          throw error
        }
      })
      if (!signal) return connecting
      return new Promise((resolve, reject) => {
        const abort = () => { stopped = true; port.removeEventListener?.('disconnect', disconnected); cleanup(); reject(cancelled(signal)) }
        const cleanup = () => signal.removeEventListener('abort', abort)
        signal.addEventListener('abort', abort, { once: true })
        connecting.then(value => { cleanup(); resolve(value) }, error => { cleanup(); reject(error) })
        if (signal.aborted) abort()
      })
    },
    writeBytes(bytes) {
      return ordered(async () => { check(); if (!writer) throw failure('SERIAL_CLOSED', 'Browser serial writer is unavailable'); await writer.write(bytes) })
    },
    setBaudRate(speed, { signal } = {}) {
      if (!Number.isSafeInteger(speed) || speed <= 0) return Promise.reject(new RangeError('baudRate must be a positive integer'))
      return ordered(async () => {
        check(signal)
        if (opened && baudRate === speed) return
        // Releasing both locks is required by Web Serial before close. Keep
        // subscribers attached; this internal reopen is not a device removal.
        reopening = true
        try { await closePort(); await open(speed, signal) }
        finally { reopening = false }
      })
    },
    disconnect() {
      stopped = true
      port.removeEventListener?.('disconnect', disconnected)
      // Web Serial cannot cancel open(). Do not block cancellation on it; the
      // late completion checks stopped and closes its newly acquired port.
      if (opening && !opened) return Promise.resolve()
      return ordered(closePort)
    },
    getDiagnostics() { return { baudRate, connected: opened, reopening } },
  }
}

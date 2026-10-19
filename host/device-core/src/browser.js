// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

/** Passive browser ROM adapter. Request the SerialPort in the UI's click handler. */
export function openBrowserRom({ port, logger = {}, signal } = {}) {
  if (!port || typeof port.open !== 'function' || typeof port.close !== 'function') throw new TypeError('A browser SerialPort is required')
  if (signal !== undefined && (!signal || typeof signal.addEventListener !== 'function' || typeof signal.aborted !== 'boolean')) throw new TypeError('signal must be an AbortSignal')
  let raw, initialized, closing = false, cleanup
  const closed = () => Object.assign(new Error('Browser ROM adapter is closed'), { code: 'ROM_CLOSED' })
  const cancelled = () => signal.reason instanceof Error ? signal.reason : Object.assign(new Error('Firmware operation cancelled'), { name: 'AbortError' })
  const check = () => { if (closing) throw closed(); if (signal?.aborted) throw cancelled() }
  const disconnect = () => {
    closing = true
    // Opening may still be in flight. initialize closes again after acquiring it.
    if (!raw) return Promise.resolve()
    return cleanup ??= raw.disconnect()
  }
  const ready = () => {
    if (closing || !raw) throw closed()
    return raw
  }
  const adapter = {
    get chipName() { return raw?.chipName ?? null },
    get chipFamily() { return raw?.chipFamily ?? null },
    get chipRevision() { return raw?.chipRevision ?? null },
    initialize() {
      if (closing) return Promise.reject(closed())
      if (initialized) return initialized
      initialized = (async () => {
        check()
        const { createBrowserLoader } = await import('./browser_rom.js')
        check()
        let rejectAbort
        const aborted = new Promise((_, reject) => { rejectAbort = reject })
        const abort = () => { rejectAbort(cancelled()); void disconnect().catch(() => {}) }
        signal?.addEventListener('abort', abort, { once: true })
        const opening = (async () => {
          raw = await createBrowserLoader(port, {
            log: (...args) => logger.log?.(...args),
            error: (...args) => logger.error?.(...args),
            debug: (...args) => logger.debug?.(...args),
          })
          if (closing || signal?.aborted) { await disconnect(); check() }
          await raw.initialize()
          check()
        })()
        try {
          if (signal?.aborted) abort()
          await Promise.race([opening, aborted])
        } catch (error) { await disconnect().catch(() => {}); throw error }
        finally { signal?.removeEventListener('abort', abort) }
      })()
      return initialized
    },
    getSecurityInfo: () => ready().getSecurityInfo(),
    runStub: skip => ready().runStub(skip),
    hardReset: () => ready().hardReset(),
    disconnect,
  }
  return adapter
}

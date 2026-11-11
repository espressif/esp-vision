// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { prepareNodeRomPort, recoverNodeRomPort } from './node_rom_port.js'
import { listSerial } from './node_inventory.js'
import { NodeSerialPort, boundedSerial } from './node_serial.js'

const fail = (code, message = code) => Object.assign(new Error(message), { code })

/** Passive Node adapter. All ROM commands use the browser's esptool-js engine. */
export function openNodeRom({ port, signal, logger, baudRate = 'auto', onStatus } = {}, dependencies = {}) {
  if (typeof port !== 'string' || !port) throw new TypeError('Firmware update requires a local --port')
  let raw, serial, initialized = false, initializing = false, closed = false, busy = false, stub
  const lifecycle = new AbortController()
  const activeSignal = signal ? AbortSignal.any([signal, lifecycle.signal]) : lifecycle.signal
  const ready = () => {
    if (closed) throw fail('ROM_CLOSED')
    if (!initialized) throw fail('ROM_NOT_INITIALIZED')
    return raw
  }
  const closeRaw = async () => {
    await serial?.close()
    await raw?.disconnect()
    raw = serial = stub = null
    initialized = false
  }
  async function start(selected) {
    activeSignal.throwIfAborted()
    const ports = await (dependencies.listSerial ?? listSerial)({ signal: activeSignal })
    const descriptor = ports.find(item => item.path === selected.port) ?? { path: selected.port }
    const { createSerialLoader, serialRomInterface } = await import('./rom_loader.js')
    serial = (dependencies.createPort ?? (info => new NodeSerialPort(info)))(descriptor)
    const kind = serialRomInterface(serial)
    // Preserve an existing stub. Only proven UART/USJ descriptors permit reset.
    const modes = ['no_reset', ...(['uart', 'usb-jtag'].includes(kind) ? ['default_reset'] : [])]
    for (const [index, mode] of modes.entries()) {
      raw = await (dependencies.createLoader ?? createSerialLoader)(serial, logger, {
        signal: activeSignal, expectedMac: selected.expectedMac ?? undefined, baudRate, onStatus, connectMode: mode,
      })
      try {
        await boundedSerial(() => raw.initialize(), { signal: activeSignal, timeout: 30000 })
        activeSignal.throwIfAborted()
        if (selected.expectedMac && raw.chipMac !== selected.expectedMac) throw fail('ROM_IDENTITY_CHANGED')
        return
      } catch (error) {
        await serial.close(); await raw.disconnect()
        activeSignal.throwIfAborted()
        if (index === modes.length - 1 || ['ROM_IDENTITY_CHANGED', 'ROM_IDENTITY_INVALID', 'ROM_PORT_LOST'].includes(error.code)) throw error
      }
    }
  }
  const run = async operation => {
    ready()
    if (busy) throw fail('ROM_BUSY')
    busy = true
    try { return await operation() }
    catch (error) { await adapter.disconnect(); throw error }
    finally { busy = false }
  }
  const requireStub = () => {
    ready()
    if (!stub?.IS_STUB) throw fail('ROM_STUB_REQUIRED')
    return stub
  }
  const adapter = {
    get chipName() { return raw?.chipName ?? null },
    get chipFamily() { return raw?.chipFamily ?? null },
    get chipRevision() { return raw?.chipRevision ?? null },
    get chipMac() { return raw?.chipMac ?? null },
    get IS_STUB() { return initialized && !!stub?.IS_STUB },
    get interface() { return raw?.interface ?? null },
    get baudRate() { return raw?.baudRate ?? null },
    async initialize() {
      if (closed) throw fail('ROM_CLOSED')
      if (initializing || initialized) throw fail('ROM_ALREADY_INITIALIZED')
      activeSignal.throwIfAborted()
      initializing = true
      let selected
      try {
        selected = await (dependencies.preparePort ?? prepareNodeRomPort)({ port, signal: activeSignal })
        try { await start(selected) }
        catch (error) {
          await closeRaw()
          activeSignal.throwIfAborted()
          // Recovery observes a known ROM returning to the same runtime.
          // Never retry a write or repeat an earlier boot request.
          if (['ROM_IDENTITY_CHANGED', 'ROM_IDENTITY_INVALID', 'SERIAL_BACKEND_UNAVAILABLE'].includes(error.code)
              || !selected.expectedMac || selected.transition?.observed !== 'rom' || selected.transition.requested !== false) throw error
          const recovered = await (dependencies.recoverPort ?? recoverNodeRomPort)(selected, { signal: activeSignal })
          if (!recovered) throw error
          selected = recovered
          selected.transition.previousInitialization = { code: error.code ?? 'ROM_INITIALIZE_FAILED', message: error.message }
          await start(selected)
        }
        activeSignal.throwIfAborted()
        adapter.port = selected.port; adapter.usbTransition = selected.transition
        initialized = true
      } catch (error) {
        if (selected?.transition) error.transition ??= selected.transition
        await adapter.disconnect()
        throw error
      } finally { initializing = false }
    },
    getSecurityInfo: () => run(() => raw.getSecurityInfo()),
    readRegister: address => run(() => raw.readRegister(address)),
    writeRegister: (address, value, mask) => run(() => raw.writeRegister(address, value, mask)),
    hardReset: () => run(() => raw.hardReset()),
    async runStub() { return run(async () => { stub = await raw.runStub(); return adapter }) },
    flashId: () => run(() => requireStub().flashId()),
    eraseRegion: (address, size) => run(() => requireStub().eraseRegion(address, size)),
    flashData: (bytes, progress, offset) => run(() => requireStub().flashData(bytes, progress, offset)),
    async readFlash(offset, bytes, progress, options = {}) {
      if (!Number.isSafeInteger(offset) || offset < 0 || !Number.isSafeInteger(bytes) || bytes <= 0 || bytes > 0xffffffff || offset + bytes > 0x100000000) throw new RangeError('ROM read requires a positive 32-bit address range')
      if (progress !== undefined && typeof progress !== 'function') throw new TypeError('ROM read progress must be a function')
      if (!options || typeof options !== 'object' || Array.isArray(options) || Object.keys(options).some(key => !['blockSize', 'maxInFlight', 'chunkSize'].includes(key))) throw new TypeError('Unsupported ROM read option')
      if (![1024, 4096].includes(options.blockSize ?? 1024) || !Number.isSafeInteger(options.maxInFlight ?? 1) || (options.maxInFlight ?? 1) < 1 || (options.maxInFlight ?? 1) > 64) throw new RangeError('Invalid ROM read packet budget')
      if (options.chunkSize !== undefined && (!Number.isSafeInteger(options.chunkSize) || options.chunkSize <= 0 || bytes > options.chunkSize)) throw new RangeError('ROM read exceeds the requested chunk size')
      return run(async () => {
        const reader = requireStub(), data = new Uint8Array(bytes)
        // The shared engine uses one outstanding 1 KiB packet, within every
        // supported caller budget. Bound each command and verify its digest.
        for (let position = 0; position < bytes;) {
          activeSignal.throwIfAborted()
          const chunk = await reader.readFlash(offset + position, Math.min(65536, bytes - position))
          data.set(chunk, position); position += chunk.length
          progress?.(chunk, position, bytes)
        }
        return data
      })
    },
    async disconnect() {
      if (closed) return
      closed = true
      lifecycle.abort(fail('ROM_CLOSED'))
      await closeRaw()
    },
  }
  return adapter
}

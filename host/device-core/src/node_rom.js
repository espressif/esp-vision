// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { spawn } from 'node:child_process'
import { createInterface } from 'node:readline'
import { fileURLToPath } from 'node:url'
import { defaultPython } from './node_inventory.js'
import { prepareNodeRomPort, recoverNodeRomPort } from './node_rom_port.js'

export function openNodeRom({ port, python = defaultPython, signal } = {}, dependencies = {}) {
  if (!port) throw new TypeError('Firmware update requires a local --port')
  const lifecycle = new AbortController()
  signal = signal ? AbortSignal.any([signal, lifecycle.signal]) : lifecycle.signal
  let child, input, pending, sequence = 0, closed = false, initializing = false, romReady = false
  const fail = code => Object.assign(new Error(code === 'ESPTOOL_UNAVAILABLE' ? 'Install Python esptool==5.6.0 for the built-in flasher' : 'ROM operation failed'), { code })
  const reject = code => { if (pending) { clearTimeout(pending.timer); pending.reject(fail(code)); pending = null } }
  function rpc(method, args = []) {
    if (closed) return Promise.reject(fail('ROM_CLOSED'))
    if (!child || (method !== 'initialize' && !romReady)) return Promise.reject(fail('ROM_NOT_INITIALIZED'))
    if (pending) return Promise.reject(fail('ROM_BUSY'))
    return new Promise((resolve, rejectPromise) => {
      const id = ++sequence
      pending = { id, resolve, reject: rejectPromise, timer: setTimeout(() => { reject('ROM_TIMEOUT'); void loader.disconnect() }, 180000) }
      child.stdin.write(JSON.stringify({ id, method, args }) + '\n', error => { if (error) reject('ROM_IO_FAILED') })
    })
  }
  async function closeProcess() {
    const process = child
    child = null; romReady = false; reject('ROM_CLOSED'); input?.close(); input = null
    if (!process || process.exitCode !== null) return
    await new Promise(resolve => {
      const timer = setTimeout(() => { process.kill(); resolve() }, 2000)
      process.once('exit', () => { clearTimeout(timer); resolve() })
      process.stdin.end()
    })
  }
  function startProcess(selected) {
    signal.throwIfAborted()
    const process = (dependencies.spawn ?? spawn)(python, ['-u', fileURLToPath(new URL('./node_rom.py', import.meta.url)), selected.port], { stdio: ['pipe', 'pipe', 'ignore'], windowsHide: true })
    child = process
    const active = action => { if (child === process) action() }
    process.on('error', () => active(() => reject('ESPTOOL_UNAVAILABLE')))
    process.on('exit', () => active(() => reject('ROM_CLOSED')))
    process.stdin.on('error', () => active(() => reject('ROM_IO_FAILED')))
    input = createInterface({ input: process.stdout })
    input.on('line', line => {
      if (child !== process) return
      try {
        const message = JSON.parse(line)
        if (message.error && !message.id) return reject(message.error)
        if (!pending || message.id !== pending.id) return
        const operation = pending; pending = null; clearTimeout(operation.timer)
        if (message.error) operation.reject(Object.assign(fail(message.error), { operation: message.method, cause: message.detail })); else operation.resolve(message.result)
      } catch { reject('ROM_RESPONSE_INVALID') }
    })
    return rpc('initialize')
  }
  const loader = {
    async initialize() {
      signal.throwIfAborted()
      if (closed) throw fail('ROM_CLOSED')
      if (initializing || child) throw fail('ROM_ALREADY_INITIALIZED')
      initializing = true
      let selected
      const cancelled = () => { reject('CANCELLED'); void loader.disconnect() }
      signal.addEventListener('abort', cancelled, { once: true })
      try {
        selected = await (dependencies.preparePort ?? prepareNodeRomPort)({ port, python, signal })
        signal.throwIfAborted()
        let identity
        try { identity = await startProcess(selected) }
        catch (error) {
          await closeProcess()
          signal.throwIfAborted()
          // No writes have been accepted. A ROM/stub can return to its app
          // between enumeration and SYNC. Recover only after observing that
          // same physical device's runtime; never repeat a previous handoff.
          if (error.code !== 'ROM_OPERATION_FAILED' || error.operation !== 'initialize'
              || !selected.expectedMac || selected.transition?.observed !== 'rom'
              || selected.transition.requested !== false) throw error
          const recovered = await (dependencies.recoverPort ?? recoverNodeRomPort)(selected, { python, signal })
          if (!recovered) throw error
          selected = recovered
          selected.transition.previousInitialization = { code: error.code, operation: error.operation, detail: String(error.cause ?? '').slice(0, 1024) }
          signal.throwIfAborted()
          identity = await startProcess(selected)
        }
        if (selected.expectedMac && identity.chipMac !== selected.expectedMac) {
          throw Object.assign(new Error('ROM MAC differs from the selected USB device'), { code: 'ROM_IDENTITY_CHANGED' })
        }
        Object.assign(loader, identity, { port: selected.port, usbTransition: selected.transition })
        romReady = true
      } catch (error) {
        if (error && typeof error === 'object' && selected?.transition) error.transition ??= selected.transition
        await loader.disconnect()
        throw error
      } finally { initializing = false; signal.removeEventListener('abort', cancelled) }
    },
    flashId: () => rpc('flashId'),
    getSecurityInfo: () => rpc('security'),
    async runStub() { await rpc('stub'); return loader },
    async readFlash(offset, bytes, progress, options) {
      if (!Number.isSafeInteger(offset) || offset < 0 || !Number.isSafeInteger(bytes) || bytes <= 0 || bytes > 0xffffffff
          || offset + bytes > 0x100000000) throw new RangeError('ROM read requires a positive 32-bit address range')
      if (progress !== undefined && typeof progress !== 'function') throw new TypeError('ROM read progress must be a function')
      if (options !== undefined && (!options || typeof options !== 'object' || Array.isArray(options))) throw new TypeError('ROM read options must be an object')
      if (options && Object.keys(options).some(key => !['blockSize', 'maxInFlight', 'chunkSize'].includes(key))) throw new TypeError('Unsupported ROM read option')
      const blockSize = options?.blockSize === undefined ? 1024 : options.blockSize
      const maxInFlight = options?.maxInFlight === undefined ? 1 : options.maxInFlight
      if (![1024, 4096].includes(blockSize) || !Number.isSafeInteger(maxInFlight) || maxInFlight < 1 || maxInFlight > 64) throw new RangeError('ROM read requires 1024 or 4096 byte blocks and 1..64 outstanding packets')
      if (options?.chunkSize !== undefined && (!Number.isSafeInteger(options.chunkSize) || options.chunkSize <= 0 || bytes > options.chunkSize)) throw new RangeError('ROM read exceeds the requested chunk size')
      const data = new Uint8Array(Buffer.from(await rpc('read', [offset, bytes, { blockSize, maxInFlight }]), 'base64'))
      if (data.length !== bytes) throw Object.assign(fail('ROM_READ_LENGTH'), { operation: 'read' })
      progress?.(data, data.length, bytes)
      return data
    },
    async flashData(bytes, progress, offset) { await rpc('write', [offset, Buffer.from(bytes).toString('base64')]); progress(bytes.byteLength, bytes.byteLength) },
    readRegister: address => rpc('readRegister', [address]),
    writeRegister: (address, value, mask) => rpc('writeRegister', [address, value, mask]),
    hardReset: () => rpc('reset'),
    async disconnect() {
      if (closed) return
      closed = true; lifecycle.abort(fail('ROM_CLOSED'))
      await closeProcess()
    },
  }
  return loader
}

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { readFile, realpath } from 'node:fs/promises'
import { basename, dirname } from 'node:path'
import { setSerialSignals } from './node_signals.js'

export const serialError = (code, message, cause) => Object.assign(new Error(message, { cause }), { code })
export function serialFailure(error, stage) {
  const code = ['EACCES', 'EPERM'].includes(error?.code) || /access denied|permission denied/i.test(error?.message ?? '') ? 'SERIAL_ACCESS_DENIED'
    : error?.code === 'EBUSY' || /cannot lock|resource busy/i.test(error?.message ?? '') ? 'SERIAL_BUSY' : 'SERIAL_ERROR'
  const result = serialError(code, `Serial ${stage} failed; check the device, cable and port permissions`, error)
  result.details = { stage, type: error?.name ?? 'Error', code: error?.code ?? null }
  return result
}

let bindingPromise
export function serialBinding() {
  return bindingPromise ??= import('@serialport/bindings-cpp').then(module => (module.autoDetect ?? module.default?.autoDetect)()).catch(cause => {
    bindingPromise = null
    throw serialError('SERIAL_BACKEND_UNAVAILABLE', 'The npm serial backend is unavailable for this platform; reinstall with optional dependencies enabled', cause)
  })
}

export async function boundedSerial(operation, { signal, timeout = 10000 } = {}) {
  signal?.throwIfAborted()
  let timer, abort
  const stopped = new Promise((_, reject) => {
    timer = setTimeout(() => reject(serialError('TIMEOUT', 'Serial operation timed out')), timeout)
    abort = () => reject(signal.reason ?? serialError('CANCELLED', 'Serial operation cancelled'))
    signal?.addEventListener('abort', abort, { once: true })
  })
  try { return await Promise.race([Promise.resolve().then(operation), stopped]) }
  finally { clearTimeout(timer); signal?.removeEventListener('abort', abort) }
}

const usbId = value => typeof value === 'string' && /^(?:0x)?[0-9a-f]{1,4}$/i.test(value) ? parseInt(value, 16)
  : Number.isInteger(value) && value >= 0 && value <= 65535 ? value : null

// Linux's npm binding does not expose USB topology. Read the USB ancestor from
// sysfs so a runtime -> ROM re-enumeration remains tied to the physical socket.
export async function serialPortInfo(port, { platform = process.platform, resolvePath = realpath, read = readFile } = {}) {
  const result = { path: port.path, vendorId: usbId(port.vendorId), productId: usbId(port.productId),
    serialNumber: port.serialNumber ?? null, manufacturer: port.manufacturer ?? null,
    product: port.product ?? port.friendlyName ?? null, location: port.locationId ?? null, hwid: port.pnpId ?? null }
  if (platform === 'linux' && /^\/dev\/[^/]+$/.test(port.path)) {
    try {
      let directory = await resolvePath(`/sys/class/tty/${basename(port.path)}/device`)
      while (directory.startsWith('/sys/')) {
        try {
          const vid = usbId((await read(`${directory}/idVendor`, 'utf8')).trim())
          if (vid !== null) {
            result.location = basename(directory)
            result.vendorId = vid
            for (const [file, field] of [['idProduct', 'productId'], ['serial', 'serialNumber'], ['manufacturer', 'manufacturer'], ['product', 'product']]) {
              try { const value = (await read(`${directory}/${file}`, 'utf8')).trim(); result[field] = file === 'idProduct' ? usbId(value) : value }
              catch { /* Some USB descriptors are optional. */ }
            }
            break
          }
        } catch { /* A tty/interface ancestor is not yet the USB device. */ }
        directory = dirname(directory)
      }
    } catch { /* A removed port cannot authorize a later identity handoff. */ }
  }
  return result
}

/** Web Serial-shaped npm port for the shared esptool-js engine. */
export class NodeSerialPort {
  constructor(info, { binding = serialBinding, signals = { dtr: false, rts: false } } = {}) {
    this.info = info; this.getBinding = binding; this.signals = { ...signals }
    this.readable = null; this.writable = null; this.handle = null; this.opening = false; this.generation = 0
  }
  getInfo() { return { usbVendorId: this.info.vendorId ?? undefined, usbProductId: this.info.productId ?? undefined } }
  async open({ baudRate = 115200 } = {}) {
    if (this.handle || this.opening) throw serialError('SERIAL_BUSY', 'Serial port is already open')
    this.opening = true
    const generation = ++this.generation
    try {
      const binding = await this.getBinding()
      const handle = await binding.open({ path: this.info.path, baudRate, lock: true, hupcl: false, rtscts: false })
      if (generation !== this.generation) { await handle.close(); throw serialError('SERIAL_CLOSED', 'Serial open cancelled') }
      this.handle = handle
      await setSerialSignals(handle, this.signals)
      if (generation !== this.generation) throw serialError('SERIAL_CLOSED', 'Serial open cancelled')
      this.readable = new ReadableStream({
        pull: async controller => {
          const buffer = Buffer.allocUnsafe(8192)
          try {
            const { bytesRead } = await handle.read(buffer, 0, buffer.length)
            if (generation === this.generation) controller.enqueue(new Uint8Array(buffer.subarray(0, bytesRead)))
          } catch (error) {
            if (generation === this.generation) { controller.error(serialFailure(error, 'read')); await this.close() }
          }
        },
        cancel: () => this.close(),
      }, { highWaterMark: 1 })
      this.writable = new WritableStream({ write: bytes => handle.write(Buffer.from(bytes)), abort: () => this.close() })
    } catch (error) { await this.close(); throw error }
    finally { this.opening = false }
  }
  async setSignals(signals) {
    if (!this.handle) throw serialError('SERIAL_CLOSED', 'Serial port is closed')
    const next = { ...this.signals }
    if ('dataTerminalReady' in signals) next.dtr = signals.dataTerminalReady
    if ('requestToSend' in signals) next.rts = signals.requestToSend
    if ('break' in signals) next.brk = signals.break
    await setSerialSignals(this.handle, next)
    this.signals = next
  }
  async setBaudRate(baudRate) {
    if (!Number.isSafeInteger(baudRate) || baudRate <= 0) throw new RangeError('baudRate must be positive')
    if (!this.handle) throw serialError('SERIAL_CLOSED', 'Serial port is closed')
    await this.handle.update({ baudRate })
  }
  async close() {
    ++this.generation
    const handle = this.handle
    this.handle = null; this.readable = null; this.writable = null
    if (handle?.isOpen) await handle.close()
  }
}

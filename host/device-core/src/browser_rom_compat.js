// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { romMd5 } from './rom_md5.js'

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms))
export const romError = (code, message, cause) => Object.assign(new Error(message, { cause }), { code })
export function romWords(...values) {
  const bytes = new Uint8Array(values.length * 4), view = new DataView(bytes.buffer)
  values.forEach((value, index) => view.setUint32(index * 4, value, true))
  return bytes
}

// Per-instance fixes for esptool-js 0.7.1, not patches to global prototypes.
// Its parser loses a whole buffered chunk (including a valid packet) when a
// boot-log prefix precedes SLIP. Keep the upstream buffer/readLoop and framing,
// while bounding a complete packet by one deadline, including slow partial data.
export function browserTransportCompatibility(transport) {
  transport.read = async function (timeout) {
    const deadline = Date.now() + timeout
    let started = false, escaping = false
    const packet = []
    for (;;) {
      if (Date.now() >= deadline) throw romError('ROM_READ_TIMEOUT', 'Timed out receiving a complete ROM packet')
      if (!this.buffer.length) {
        if (!this.device.readable) throw romError('ROM_PORT_LOST', 'Serial device disconnected; select its current download port again')
        await sleep(Math.min(2, Math.max(1, deadline - Date.now())))
        continue
      }
      const bytes = this.buffer
      this.buffer = new Uint8Array(0)
      if (!started && bytes[0] !== 0xc0) this.detectPanicHandler?.(bytes)
      for (let i = 0; i < bytes.length; i++) {
        const byte = bytes[i]
        if (!started) {
          if (byte === 0xc0) started = true
        } else if (escaping) {
          escaping = false
          if (byte !== 0xdc && byte !== 0xdd) throw romError('ROM_SLIP_ESCAPE', `Invalid SLIP escape (0xdb, 0x${byte.toString(16)})`)
          packet.push(byte === 0xdc ? 0xc0 : 0xdb)
        } else if (byte === 0xdb) escaping = true
        else if (byte === 0xc0) {
          this.buffer = this.appendArray(bytes.slice(i + 1), this.buffer)
          if (packet.length) return new Uint8Array(packet)
          started = true
        } else packet.push(byte)
        if (packet.length > 65536) throw romError('ROM_PACKET_SIZE', 'ROM packet exceeds the bounded receive size')
      }
    }
  }
  const write = transport.write
  transport.write = async function (bytes) {
    if (!this.device.writable) throw romError('ROM_PORT_LOST', 'Serial device disconnected; select its current download port again')
    if (typeof this.slipWriter !== 'function') return write.call(this, bytes)
    const writer = this.device.writable.getWriter()
    try { await writer.write(this.slipWriter(bytes)) }
    finally { writer.releaseLock() }
  }
  return transport
}

// #273 remains an open timing investigation. This deliberately bounded read
// uses 1 KiB blocks and one outstanding packet, then consumes AND verifies the
// protocol's final MD5 packet. Replace this shim when a pinned upstream release
// implements these semantics and passes read -> write -> full readback coverage.
export async function readBrowserFlash(loader, address, size, progress, { timeout = 5000 } = {}) {
  if (!loader.IS_STUB) throw romError('ROM_STUB_REQUIRED', 'Readback requires an initialized flasher stub')
  if (!Number.isSafeInteger(address) || address < 0 || !Number.isSafeInteger(size) || size <= 0 || size > 65536 || address + size > 0x100000000) throw new RangeError('ROM read requires a positive range of at most 64 KiB')
  const status = await loader.checkCommand('read flash', loader.ESP_READ_FLASH, romWords(address, size, 1024, 1), 0, 0, timeout)
  if (status !== 0) throw romError('ROM_READ_STATUS', `Flash read command failed: ${status}`)
  const output = new Uint8Array(size)
  let received = 0
  const deadline = Date.now() + Math.max(timeout, Math.ceil(size * 10 * 1000 / (loader.transport.baudrate || 115200)) + timeout)
  const read = () => loader.transport.read(Math.max(1, Math.min(timeout, deadline - Date.now())))
  while (received < size) {
    if (Date.now() >= deadline) throw romError('ROM_READ_TIMEOUT', 'Flash readback exceeded its deadline')
    const packet = await read()
    if (!(packet instanceof Uint8Array) || !packet.length || packet.length > 1024 || received + packet.length > size) throw romError('ROM_READ_PACKET', 'Invalid or oversized flash readback packet')
    output.set(packet, received); received += packet.length
    await loader.transport.write(romWords(received))
    progress?.(packet, received, size)
  }
  const digest = await read()
  if (!(digest instanceof Uint8Array) || digest.length !== 16) throw romError('ROM_READ_DIGEST', 'Missing or malformed flash readback MD5 digest')
  const expected = romMd5(output)
  if (digest.some((byte, index) => byte !== expected[index])) throw romError('ROM_READ_DIGEST', 'Flash readback MD5 digest mismatch')
  return output
}

export function browserRomCompatibility(loader, { nativeUsb = false } = {}) {
  const finish = loader.memFinish
  if (nativeUsb) loader.memFinish = async function (...args) {
    try { return await finish.apply(this, args) }
    catch (error) {
      // Some native-USB ROMs omit MEM_END acknowledgement. runStub must still
      // consume and validate the exact OHAI greeting before setting IS_STUB.
      if (!/timed?\s*out|timeout|serial data stream stopped|no serial data received/i.test(error.message)) throw error
    }
  }
  return loader
}

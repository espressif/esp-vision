// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { spawn } from 'node:child_process'
import { createInterface } from 'node:readline'
import { fileURLToPath } from 'node:url'
import { defaultPython } from './node_inventory.js'

export function openNodeRom({ port, python = defaultPython, signal } = {}) {
  if (!port) throw new TypeError('Firmware update requires a local --port')
  let child, input, pending, sequence = 0, closed = false
  const fail = code => Object.assign(new Error(code === 'ESPTOOL_UNAVAILABLE' ? 'Install Python esptool>=5.3,<6 for the built-in flasher' : 'ROM operation failed'), { code })
  const reject = code => { if (pending) { clearTimeout(pending.timer); pending.reject(fail(code)); pending = null } }
  function rpc(method, args = []) {
    if (closed) return Promise.reject(fail('ROM_CLOSED'))
    if (pending) return Promise.reject(fail('ROM_BUSY'))
    return new Promise((resolve, rejectPromise) => {
      const id = ++sequence
      pending = { id, resolve, reject: rejectPromise, timer: setTimeout(() => { reject('ROM_TIMEOUT'); void loader.disconnect() }, 180000) }
      child.stdin.write(JSON.stringify({ id, method, args }) + '\n', error => { if (error) reject('ROM_IO_FAILED') })
    })
  }
  const loader = {
    async initialize() {
      signal?.throwIfAborted()
      child = spawn(python, ['-u', fileURLToPath(new URL('./node_rom.py', import.meta.url)), port], { stdio: ['pipe', 'pipe', 'ignore'], windowsHide: true })
      child.on('error', () => reject('ESPTOOL_UNAVAILABLE'))
      child.on('exit', () => reject('ROM_CLOSED'))
      child.stdin.on('error', () => reject('ROM_IO_FAILED'))
      input = createInterface({ input: child.stdout })
      input.on('line', line => {
        try {
          const message = JSON.parse(line)
          if (message.error && !message.id) return reject(message.error)
          if (!pending || message.id !== pending.id) return
          const operation = pending; pending = null; clearTimeout(operation.timer)
          if (message.error) operation.reject(Object.assign(fail(message.error), { operation: message.method, cause: message.detail })); else operation.resolve(message.result)
        } catch { reject('ROM_RESPONSE_INVALID') }
      })
      const cancelled = () => { reject('CANCELLED'); void loader.disconnect() }
      signal?.addEventListener('abort', cancelled, { once: true })
      try { Object.assign(loader, await rpc('initialize')) }
      finally { signal?.removeEventListener('abort', cancelled) }
    },
    flashId: () => rpc('flashId'),
    getSecurityInfo: () => rpc('security'),
    async runStub() { await rpc('stub'); return loader },
    async readFlash(offset, bytes) { return new Uint8Array(Buffer.from(await rpc('read', [offset, bytes]), 'base64')) },
    async flashData(bytes, progress, offset) { await rpc('write', [offset, Buffer.from(bytes).toString('base64')]); progress(bytes.byteLength, bytes.byteLength) },
    readRegister: address => rpc('readRegister', [address]),
    writeRegister: (address, value, mask) => rpc('writeRegister', [address, value, mask]),
    hardReset: () => rpc('reset'),
    async disconnect() {
      if (closed) return
      closed = true; reject('ROM_CLOSED'); input?.close()
      if (!child || child.exitCode !== null) return
      await new Promise(resolve => {
        const timer = setTimeout(() => { child.kill(); resolve() }, 2000)
        child.once('exit', () => { clearTimeout(timer); resolve() })
        child.stdin.end()
      })
    },
  }
  return loader
}

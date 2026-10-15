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
  let child
  return {
    subscribe: events.subscribe,
    connect({ signal }) {
      return new Promise((resolveConnect, reject) => {
        const adapter = fileURLToPath(new URL('./node_serial.py', import.meta.url))
        child = spawn(options.python, ['-u', adapter, options.serial], { stdio: ['pipe', 'pipe', 'pipe'] })
        let ready = false, stderr = ''
        const abort = () => { child.kill('SIGTERM'); reject(fail('CANCELLED', 'Serial connection cancelled')) }
        const cleanup = () => signal.removeEventListener('abort', abort)
        const error = () => { cleanup(); const code = ['SERIAL_ACCESS_DENIED', 'SERIAL_BUSY'].find(code => stderr.includes(code)) ?? 'SERIAL_ERROR'; const error = fail(code, 'Serial adapter failed; ensure Python, pyserial and port access are available'); reject(error); events.emit('error', error) }
        child.on('error', error)
        child.stdin.on('error', error)
        child.stdout.on('data', bytes => events.emit('data', bytes))
        child.stderr.on('data', bytes => {
          if (ready) return
          // Do not print Python errors: they can contain host paths or device data.
          stderr = (stderr + bytes.toString()).slice(-128)
          if (stderr.split(/\r?\n/).includes('READY')) { ready = true; cleanup(); resolveConnect() }
        })
        child.on('exit', () => { cleanup(); if (!ready) error(); events.emit('close') })
        signal.addEventListener('abort', abort, { once: true })
        if (signal.aborted) abort()
      })
    },
    writeBytes(bytes) { return new Promise((resolveWrite, reject) => child?.stdin.writable ? child.stdin.write(bytes, error => error ? reject(fail('SERIAL_WRITE', 'Serial write failed')) : resolveWrite()) : reject(fail('SERIAL_CLOSED', 'Serial adapter closed'))) },
    async disconnect() {
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

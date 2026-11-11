// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
let ioctlPromise
async function linuxIoctl() {
  return ioctlPromise ??= import('koffi').then(({ default: koffi }) => {
    const libc = koffi.load(null)
    const ioctl = libc.func('int ioctl(int fd, unsigned long request, _Inout_ int *value)')
    return (fd, request, value) => {
      if (ioctl(fd, request, value) < 0) throw Object.assign(new Error('Serial modem control failed'), { errno: koffi.errno() })
    }
  }).catch(error => { ioctlPromise = null; throw error })
}

// bindings-cpp clears BREAK before setting DTR/RTS. Linux USB-Serial-JTAG
// rejects that unrelated ioctl, preventing the subsequent modem-line update.
// Use the same exclusively owned descriptor, preserving unrelated modem bits.
export async function setSerialSignals(handle, signals, { platform = process.platform, getIoctl = linuxIoctl } = {}) {
  try { await handle.set(signals) }
  catch (error) {
    if (platform !== 'linux' || !Number.isInteger(handle.fd) || signals.brk === true ||
        !/Operation not supported, cannot set/.test(error.message)) throw error
    const ioctl = await getIoctl()
    if (!handle.isOpen) throw new Error('Serial port closed during modem control')
    const bits = [0]
    ioctl(handle.fd, 0x5415, bits) // TIOCMGET
    bits[0] = (bits[0] & ~0x6) | (signals.dtr ? 0x2 : 0) | (signals.rts ? 0x4 : 0)
    ioctl(handle.fd, 0x5418, bits) // TIOCMSET
  }
}

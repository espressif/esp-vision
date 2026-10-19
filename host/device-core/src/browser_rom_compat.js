// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Compatibility for pinned tasmota-webserial-esptool 9.4.2. Keep changes per
// instance: consumers need neither package-manager patches nor global prototypes.
export function browserRomCompatibility(loader) {
  const readPacket = loader.readPacket
  loader.readPacket = async function (timeout) {
    const deadline = Date.now() + timeout
    for (;;) {
      try { return await readPacket.call(this, Math.max(1, deadline - Date.now())) }
      catch (error) {
        // Only a byte before the SLIP header is safe to discard. Preserve invalid
        // escapes, mid-packet errors, I/O failures and the overall timeout.
        if (!/^Invalid head of packet \(/.test(error.message) || Date.now() >= deadline) throw error
      }
    }
  }
  const runStub = loader.runStub
  loader.runStub = async function (skipFlashDetection = false) {
    const memFinish = this.memFinish, packet = this.readPacket
    const nativeUsb = this.isUsbJtagOrOtg === true
    if (nativeUsb) {
      // USB can reset before ROM sends MEM_END's reply. Do not claim success:
      // upstream must still receive and validate OHAI from the running stub.
      this.memFinish = async (...args) => {
        try { return await memFinish.apply(this, args) }
        catch (error) {
          if (!/timed?\s*out|timeout/i.test(error.message)) throw error
        }
      }
      this.readPacket = timeout => packet.call(this, Math.max(timeout, 5000))
    }
    try {
      const stub = await runStub.call(this, skipFlashDetection)
      if (!stub.IS_STUB) throw new Error('This device has no supported browser flash stub')
      browserStubCompatibility(stub)
      return stub
    } finally { this.memFinish = memFinish; this.readPacket = packet }
  }
  return loader
}

function browserStubCompatibility(stub) {
  // Official esp-flasher-stub erases the entire region in FLASH_BEGIN. Its erase
  // duration must scale with image size rather than the short command timeout.
  const command = stub.checkCommand
  stub.checkCommand = function (opcode, buffer, checksum, timeout) {
    if (opcode === 0x02 && buffer?.length >= 4) {
      const size = (buffer[0] | buffer[1] << 8 | buffer[2] << 16 | buffer[3] << 24) >>> 0
      timeout = Math.max(timeout ?? 0, 3000, 30000 * size / 1000000)
    }
    return command.call(this, opcode, buffer, checksum, timeout)
  }
  // Stub commands use their own loader instance and inherit the same SLIP issue.
  const readPacket = stub.readPacket
  stub.readPacket = async function (timeout) {
    const deadline = Date.now() + timeout
    for (;;) {
      try { return await readPacket.call(this, Math.max(1, deadline - Date.now())) }
      catch (error) {
        if (!/^Invalid head of packet \(/.test(error.message) || Date.now() >= deadline) throw error
      }
    }
  }
}

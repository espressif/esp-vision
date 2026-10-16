// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
// ROM flashing is separate from DeviceSession and its EV-MUX v4 owner protocol.
export async function firmwareSha256(bytes) {
  const digest = await globalThis.crypto.subtle.digest('SHA-256', bytes)
  return Array.from(new Uint8Array(digest), byte => byte.toString(16).padStart(2, '0')).join('')
}

export function firmwareMode(mode = 'update') {
  if (!['update', 'install'].includes(mode)) throw new TypeError('Firmware mode must be update or install')
  return mode
}

export async function validateFirmwareImage(image, bytes, { mode = 'update' } = {}) {
  firmwareMode(mode)
  if (image.evMuxVersion !== 4) throw new Error('Only approved EV-MUX v4 firmware can be installed')
  if (!(bytes instanceof Uint8Array) || bytes.byteLength !== image.bytes) throw new Error('Firmware image size does not match the approved build')
  const limit = mode === 'install' ? image.flashSize : image.partitionSize
  if (!Number.isSafeInteger(image.offset) || !Number.isSafeInteger(limit)
      || image.offset < 0 || image.bytes <= 0 || image.bytes > limit || (mode === 'install' && image.offset !== 0)) throw new Error('Invalid firmware image bounds')
  if (await firmwareSha256(bytes) !== image.sha256) throw new Error('Firmware SHA-256 does not match the approved build')
  if (mode === 'install') {
    const segments = [image.bootloader, image.partitionTable, image.application]
    let end = 0
    for (const segment of segments) {
      if (!segment || !Number.isSafeInteger(segment.offset) || !Number.isSafeInteger(segment.bytes)
          || segment.offset < end || segment.bytes <= 0 || segment.offset + segment.bytes > bytes.byteLength
          || !/^[a-f0-9]{64}$/.test(segment.sha256 ?? '')) throw new Error('Complete image requires ordered, non-overlapping bootloader, partition table and application metadata')
      if (await firmwareSha256(bytes.subarray(segment.offset, segment.offset + segment.bytes)) !== segment.sha256) throw new Error('Complete image does not contain the approved bootloader, partition table and application')
      end = segment.offset + segment.bytes
    }
  }
  return bytes
}

/** Loader injected by the browser host; no Web Serial or ROM protocol duplicated. */
export class FirmwareFlashSession {
  constructor(loader, image, { reset = () => loader.hardReset(), mode = 'update' } = {}) {
    this.mode = firmwareMode(mode)
    this.loader = loader
    this.reset = reset
    this.image = image
    this.state = 'idle'
    this.stub = null
  }

  async connect(bytes) {
    if (this.state !== 'idle') throw new Error('Firmware loader is already in use')
    this.state = 'connecting'
    try {
      await validateFirmwareImage(this.image, bytes, { mode: this.mode })
      await this.loader.initialize()
      const revisionMatches = this.image.chipRevision !== undefined ? this.loader.chipRevision === this.image.chipRevision
        : Number.isInteger(this.image.chipRevisionMin) && Number.isInteger(this.image.chipRevisionMax) && this.loader.chipRevision >= this.image.chipRevisionMin && this.loader.chipRevision <= this.image.chipRevisionMax
      if (this.loader.chipFamily !== this.image.chipFamily || !revisionMatches) {
        throw new Error('Chip or silicon revision does not match the approved board firmware')
      }
      const security = await this.loader.getSecurityInfo()
      if (!Number.isSafeInteger(security.flags) || !Number.isSafeInteger(security.flashCryptCnt)) {
        throw new Error('Cannot qualify the device ROM security mode')
      }
      const encrypted = security.flashCryptCnt.toString(2).replace(/0/g, '').length % 2 === 1
      if ((security.flags & 1) !== 0 || encrypted) throw new Error('Secure boot or encrypted devices require their signed flashing workflow')
      this.stub = await this.loader.runStub(true)
      if (this.mode === 'install') {
        // JEDEC capacity byte encodes log2(bytes); shared with browser loaders.
        if (typeof this.stub.flashId !== 'function') throw new Error('Installation requires ROM flash capacity detection (flashId)')
        const id = await this.stub.flashId()
        const capacity = (id >>> 16) & 0xff
        if (!Number.isSafeInteger(id) || id <= 0 || id > 0xffffff || capacity < 18 || capacity > 30
            || 2 ** capacity < this.image.flashSize) throw new Error('Detected flash is unknown or smaller than the approved complete image layout')
      } else {
        const table = this.image.partitionTable
        const existing = await this.stub.readFlash(table.offset, table.bytes)
        if (await firmwareSha256(existing) !== table.sha256) throw new Error('Partition layout differs; select explicit install mode to replace the layout')
      }
      this.state = 'connected'
    } catch (error) { await this.disconnect(); throw error }
  }

  async flash(bytes, { onProgress = () => {}, onPhase = () => {} } = {}) {
    if (this.state !== 'connected') throw new Error('Connect and validate the device before flashing')
    this.state = 'flashing'
    try {
      await validateFirmwareImage(this.image, bytes, { mode: this.mode })
      onPhase(this.mode === 'install' ? 'Installing complete firmware; existing data in the written range will be replaced' : 'Writing application; bootloader, partition table, NVS and user files are preserved')
      await this.stub.flashData(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
        (written, total) => onProgress(total ? Math.min(80, Math.round(written / total * 80)) : 0), this.image.offset, false)
      onPhase(this.mode === 'install' ? 'Verifying the complete installation image' : 'Verifying the complete application image')
      // maxInFlight is a packet count: one 1 KiB packet bounds USB-JTAG backlog.
      const readback = new Uint8Array(bytes.byteLength)
      for (let offset = 0; offset < bytes.byteLength; offset += 65536) {
        const count = Math.min(65536, bytes.byteLength - offset)
        const chunk = await this.stub.readFlash(this.image.offset + offset, count, undefined,
          { chunkSize: 65536, blockSize: 1024, maxInFlight: 1 })
        if (chunk.byteLength !== count) throw new Error('Written firmware readback is incomplete')
        readback.set(chunk, offset)
        onProgress(80 + Math.floor(20 * (offset + count) / bytes.byteLength))
      }
      if (readback.byteLength !== bytes.byteLength || await firmwareSha256(readback) !== this.image.sha256) throw new Error('Written firmware verification failed; keep the device in download mode for recovery')
      this.state = 'verified'
      try {
        // P4 software download requests survive a CPU reset. Clear only that
        // latch, verify all other live fields, then apply the host reset policy.
        if (this.loader.chipFamily === 0x3280) {
          const address = 0x50110008, mask = 4
          const before = await this.stub.readRegister(address)
          if (!Number.isInteger(before) || before < 0 || before > 0xffffffff) throw new Error('Invalid P4 system control register')
          if (before & mask) await this.stub.writeRegister(address, 0, mask)
          const after = await this.stub.readRegister(address)
          if (after !== ((before & ~mask) >>> 0)) throw new Error('P4 normal boot latch verification failed')
        }
        await this.reset()
      }
      catch { onPhase('Firmware verified. Press RESET or reconnect power to start it.'); return { verified: true, needsReset: true } }
      onPhase('Firmware verified and reset. Reconnect in the IDE to verify EV-MUX v4.')
      return { verified: true, needsReset: false }
    } catch (error) { this.state = 'error'; throw error }
    finally { await this.disconnect() }
  }

  async disconnect() {
    if (this.state === 'closed') return
    this.state = 'closed'
    await this.loader.disconnect()
  }
}

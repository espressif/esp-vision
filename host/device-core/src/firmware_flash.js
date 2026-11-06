// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
// ROM flashing is separate from DeviceSession and its EV-MUX v4 owner protocol.
import { planInstallFilesystemErase } from './firmware_partitions.js'
import { romMd5Hex } from './rom_md5.js'
import { clearDownloadLatch } from './rom_reset.js'

function checkAbort(signal) {
  if (signal?.aborted) throw signal.reason instanceof Error ? signal.reason : Object.assign(new Error('Firmware operation cancelled'), { name: 'AbortError' })
}
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
  #image
  #mode
  #filesystem
  #signal

  constructor(loader, image, { reset = () => loader.hardReset(), mode = 'update', signal } = {}) {
    this.mode = firmwareMode(mode)
    this.loader = loader
    this.reset = reset
    this.image = image
    this.state = 'idle'
    this.stub = null
    this.#signal = signal
  }

  async connect(bytes) {
    if (this.state !== 'idle') throw new Error('Firmware loader is already in use')
    this.state = 'connecting'
    try {
      checkAbort(this.#signal)
      // Keep qualification independent of caller mutations during callbacks or
      // asynchronous ROM operations. Public metadata is not an erase authority.
      const image = this.#image = structuredClone(this.image)
      const mode = this.#mode = firmwareMode(this.mode)
      const payload = bytes instanceof Uint8Array ? new Uint8Array(bytes) : bytes
      await validateFirmwareImage(image, payload, { mode })
      this.#filesystem = mode === 'install' ? planInstallFilesystemErase(image, payload) : null
      checkAbort(this.#signal)
      await this.loader.initialize()
      checkAbort(this.#signal)
      const revisionMatches = image.chipRevision !== undefined ? this.loader.chipRevision === image.chipRevision
        : Number.isInteger(image.chipRevisionMin) && Number.isInteger(image.chipRevisionMax) && this.loader.chipRevision >= image.chipRevisionMin && this.loader.chipRevision <= image.chipRevisionMax
      if (this.loader.chipFamily !== image.chipFamily || !Number.isInteger(this.loader.chipRevision) || !revisionMatches) {
        throw new Error('Chip or silicon revision does not match the approved board firmware')
      }
      const security = await this.loader.getSecurityInfo()
      checkAbort(this.#signal)
      if (!Number.isSafeInteger(security.flags) || security.flags < 0 || security.flags > 0xffffffff
          || !Number.isSafeInteger(security.flashCryptCnt) || security.flashCryptCnt < 0 || security.flashCryptCnt > 0xffffffff) {
        throw new Error('Cannot qualify the device ROM security mode')
      }
      const encrypted = security.flashCryptCnt.toString(2).replace(/0/g, '').length % 2 === 1
      if ((security.flags & 5) !== 0 || encrypted) throw new Error('Secure boot, secure download or encrypted devices require their signed flashing workflow')
      this.stub = await this.loader.runStub(true)
      checkAbort(this.#signal)
      if (mode === 'install') {
        // JEDEC capacity byte encodes log2(bytes); shared with browser loaders.
        if (typeof this.stub.flashId !== 'function') throw new Error('Installation requires ROM flash capacity detection (flashId)')
        const id = await this.stub.flashId()
        checkAbort(this.#signal)
        const capacity = (id >>> 16) & 0xff
        if (!Number.isSafeInteger(id) || id <= 0 || id > 0xffffff || capacity < 18 || capacity > 30
            || 2 ** capacity < image.flashSize) throw new Error('Detected flash is unknown or smaller than the approved complete image layout')
        if (typeof this.stub.eraseRegion !== 'function' || typeof this.stub.readFlash !== 'function' || typeof this.stub.flashData !== 'function') {
          throw Object.assign(new Error('Complete installation requires filesystem erase and firmware verification support'), { code: 'FIRMWARE_ERASE_UNSUPPORTED' })
        }
      } else {
        const table = image.partitionTable
        const existing = await this.stub.readFlash(table.offset, table.bytes)
        if (await firmwareSha256(existing) !== table.sha256) throw new Error('Partition layout differs; select explicit install mode to replace the layout')
      }
      checkAbort(this.#signal)
      this.state = 'connected'
    } catch (error) { await this.disconnect(); throw error }
  }

  async flash(bytes, { onProgress = () => {}, onPhase = () => {} } = {}) {
    if (this.state !== 'connected') throw new Error('Connect and validate the device before flashing')
    this.state = 'flashing'
    try {
      checkAbort(this.#signal)
      const image = this.#image, install = this.#mode === 'install'
      const payload = bytes instanceof Uint8Array ? new Uint8Array(bytes) : bytes
      await validateFirmwareImage(image, payload, { mode: this.#mode })
      checkAbort(this.#signal)
      if (install) {
        const { offset, size } = this.#filesystem
        onPhase('Erasing internal FAT filesystem; device files will be deleted')
        checkAbort(this.#signal)
        // Erase only the range parsed from the qualified image. No whole-chip
        // erase, backups or automatic recovery/replay are part of installation.
        await this.stub.eraseRegion(offset, size)
        checkAbort(this.#signal)
        // Explicit installation trusts the checked erase command; do not read
        // or hash the erased filesystem back over the download connection.
      }
      onPhase(install ? 'Installing complete firmware; internal FAT filesystem cleared' : 'Writing application; bootloader, partition table, NVS and user files are preserved')
      checkAbort(this.#signal)
      const verification = await this.stub.flashData(payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.byteLength),
        (written, total) => onProgress(total ? Math.min(80, Math.round(written / total * 80)) : 0), image.offset, true)
      // Once firmware writing starts, preserve the existing completion policy:
      // finish verification/reset rather than interrupting a partial image.
      if (verification !== undefined && verification !== null) {
        onPhase('Checking esptool device MD5 verification')
        if (verification.algorithm !== 'md5' || verification.offset !== image.offset || verification.bytes !== payload.byteLength
            || !/^[a-f0-9]{32}$/.test(verification.digest ?? '') || verification.digest !== romMd5Hex(payload)) {
          throw new Error('Written firmware verification failed; invalid esptool MD5 receipt')
        }
      } else {
        // Custom adapters without a verified write receipt retain full readback.
        onPhase(install ? 'Verifying the complete installation image' : 'Verifying the complete application image')
        // maxInFlight is a packet count: one 1 KiB packet bounds USB-JTAG backlog.
        const readback = new Uint8Array(payload.byteLength)
        for (let offset = 0; offset < payload.byteLength; offset += 65536) {
          const count = Math.min(65536, payload.byteLength - offset)
          const chunk = await this.stub.readFlash(image.offset + offset, count, undefined,
            { chunkSize: 65536, blockSize: 1024, maxInFlight: 1 })
          if (chunk.byteLength !== count) throw new Error('Written firmware readback is incomplete')
          readback.set(chunk, offset)
          onProgress(80 + Math.min(19, Math.floor(20 * (offset + count) / payload.byteLength)))
        }
        if (await firmwareSha256(readback) !== image.sha256) throw new Error('Written firmware verification failed; keep the device in download mode for recovery')
      }
      onProgress(100)
      this.state = 'verified'
      try {
        await clearDownloadLatch(image.chipFamily, this.stub)
        await this.reset()
      }
      catch (error) {
        onPhase('Firmware verified. Automatic reset did not complete; wait for runtime reconnection or press RESET.')
        return { verified: true, needsReset: true, resetError: { code: error?.code ?? 'RESET_FAILED', message: error?.message ?? String(error) } }
      }
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

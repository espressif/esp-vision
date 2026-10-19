// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { MAX_FIRMWARE_BYTES, firmwareUrl } from './firmware_io.js'

export function validateFirmwareRelease(value, mode = 'update') {
  if (!value || typeof value.board !== 'string' || !value.board || typeof value.version !== 'string' || !value.version) throw new TypeError('Release requires board and version')
  const image = value.image
  const revisionValid = image?.chipRevision !== undefined
    ? Number.isSafeInteger(image.chipRevision) && image.chipRevision >= 0
    : Number.isSafeInteger(image?.chipRevisionMin) && image.chipRevisionMin >= 0
      && Number.isSafeInteger(image?.chipRevisionMax) && image.chipRevisionMax >= image.chipRevisionMin
  if (!image || image.evMuxVersion !== 4 || !Number.isSafeInteger(image.chipFamily)
      || image.chipFamily < 0 || !revisionValid || !Number.isSafeInteger(image.offset) || image.offset < 0
      || !Number.isSafeInteger(image.bytes) || image.bytes <= 0 || !Number.isSafeInteger(image.partitionSize) || image.bytes > image.partitionSize
      || !/^[a-f0-9]{64}$/.test(image.sha256 ?? '') || !Number.isSafeInteger(image.partitionTable?.offset) || image.partitionTable.offset < 0
      || !Number.isSafeInteger(image.partitionTable?.bytes) || image.partitionTable.bytes <= 0
      || !/^[a-f0-9]{64}$/.test(image.partitionTable?.sha256 ?? '')) throw new TypeError('Release requires complete EV-MUX v4 application/chip/partition/SHA-256 metadata')
  if (mode === 'install') {
    const install = value.install
    if (!install || install.offset !== 0 || !Number.isSafeInteger(install.bytes) || install.bytes <= 0
        || !Number.isSafeInteger(install.flashSize) || install.flashSize < install.bytes || install.flashSize > 2 ** 30
        || !Number.isInteger(Math.log2(install.flashSize)) || install.flashSize < 2 ** 18
        || !/^[a-f0-9]{64}$/.test(install.sha256 ?? '') || !install.bootloader
        || !Number.isSafeInteger(install.chipRevisionMin) || !Number.isSafeInteger(install.chipRevisionMax)
        || install.chipRevisionMin < (image.chipRevision ?? image.chipRevisionMin) || install.chipRevisionMax > (image.chipRevision ?? image.chipRevisionMax)
        || install.chipRevisionMin < 0 || install.chipRevisionMax < install.chipRevisionMin) throw Object.assign(new TypeError('Release lacks qualified complete-install metadata'), { code: 'FIRMWARE_INSTALL_UNAVAILABLE' })
    let end = 0
    for (const part of [install.bootloader, image.partitionTable, image]) {
      if (!Number.isSafeInteger(part.offset) || !Number.isSafeInteger(part.bytes) || part.bytes <= 0
          || part.offset < end || part.offset + part.bytes > install.bytes
          || !/^[a-f0-9]{64}$/.test(part.sha256 ?? '')) throw new TypeError('Invalid complete-install segment bounds or hashes')
      end = part.offset + part.bytes
    }
  }
  if (image.source !== undefined) {
    const source = image.source, install = value.install
    if (!source || typeof source !== 'object' || Array.isArray(source)
        || !Number.isSafeInteger(source.bytes) || source.bytes <= 0 || source.bytes > MAX_FIRMWARE_BYTES
        || !Number.isSafeInteger(source.offset) || source.offset < 0 || source.offset !== image.offset
        || !Number.isSafeInteger(source.offset + image.bytes) || source.offset + image.bytes > source.bytes
        || !/^[a-f0-9]{64}$/.test(source.sha256 ?? '') || source.url !== image.url
        || !install || source.url !== install.url || source.bytes !== install.bytes || source.sha256 !== install.sha256) throw new TypeError('Invalid complete BIN source bounds or install consistency')
    firmwareUrl(source.url)
    if (mode !== 'install') validateFirmwareRelease(value, 'install')
  }
  return structuredClone(value)
}

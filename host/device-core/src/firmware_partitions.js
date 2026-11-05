// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { romMd5 } from './rom_md5.js'

const SECTOR_BYTES = 4096
const invalid = message => Object.assign(new Error(`Invalid installation partition table: ${message}`), { code: 'FIRMWARE_PARTITION_INVALID' })
const allFF = bytes => bytes.every(value => value === 0xff)

// Call only after validateFirmwareImage has authenticated the complete image
// and every declared segment. Never infer an erase range from a host/UI field.
export function planInstallFilesystemErase(image, bytes) {
  const table = image.partitionTable
  if (table.offset % SECTOR_BYTES || table.bytes % 32 || table.bytes > 0xc00) throw invalid('unsupported table bounds')
  const raw = bytes.subarray(table.offset, table.offset + table.bytes)
  const view = new DataView(raw.buffer, raw.byteOffset, raw.byteLength)
  const entries = [], labels = new Set()
  let digestSeen = false, terminated = false
  for (let cursor = 0; cursor < raw.length; cursor += 32) {
    const entry = raw.subarray(cursor, cursor + 32)
    if (allFF(entry)) {
      if (!allFF(raw.subarray(cursor))) throw invalid('data after the end marker')
      terminated = true
      break
    }
    const magic = view.getUint16(cursor, true)
    if (magic === 0xebeb && !digestSeen) {
      if (!allFF(entry.subarray(2, 16))) throw invalid('malformed MD5 record')
      const digest = romMd5(raw.subarray(0, cursor))
      if (!digest.every((value, index) => value === entry[16 + index])) throw invalid('MD5 mismatch')
      digestSeen = true
      continue
    }
    if (magic !== 0x50aa || digestSeen) throw invalid('unexpected entry or data after MD5')
    const offset = view.getUint32(cursor + 4, true), size = view.getUint32(cursor + 8, true)
    const type = entry[2], subtype = entry[3], flags = view.getUint32(cursor + 28, true)
    const labelBytes = entry.subarray(12, 28), end = labelBytes.indexOf(0)
    const label = String.fromCharCode(...labelBytes.subarray(0, end < 0 ? 16 : end))
    if (!label || labels.has(label) || (end >= 0 && labelBytes.subarray(end).some(value => value !== 0))) throw invalid('empty, duplicate or malformed label')
    if (offset < table.offset + SECTOR_BYTES || offset % SECTOR_BYTES || !size
        || offset + size > image.flashSize || (type === 0 && offset % 65536)) throw invalid('unsupported partition bounds')
    labels.add(label)
    entries.push({ label, type, subtype, offset, size, flags })
  }
  if (!terminated || !entries.length) throw invalid('missing entries or end marker')
  entries.sort((a, b) => a.offset - b.offset)
  for (let index = 1; index < entries.length; index++) {
    if (entries[index].offset < entries[index - 1].offset + entries[index - 1].size) throw invalid('overlapping partitions')
  }
  const app = image.application
  if (!entries.some(entry => entry.type === 0 && entry.offset === app.offset
      && entry.size === app.partitionSize && app.bytes <= entry.size)) throw invalid('application does not match the approved layout')
  const fat = entries.find(entry => entry.label === 'ffat')
  if (!fat) throw Object.assign(new Error('Complete installation requires a declared internal ffat partition'), { code: 'FIRMWARE_FATFS_UNAVAILABLE' })
  if (fat.type !== 1 || fat.subtype !== 0x81 || fat.flags !== 0 || fat.size % SECTOR_BYTES
      || fat.offset < Math.ceil(bytes.byteLength / SECTOR_BYTES) * SECTOR_BYTES) throw invalid('ffat is not an unencrypted, writable FAT partition outside the firmware write range')
  return Object.freeze({ label: fat.label, offset: fat.offset, size: fat.size })
}

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

const invalid = reason => Object.assign(new Error(`Invalid JPEG image: ${reason}`), { code: 'INVALID_IMAGE', reason })

/** Bounded JPEG marker/table checks, not an entropy decoder (ITU-T T.81 B/C). */
export function validateJpegImage(bytes, metadata = {}) {
  if (!(bytes instanceof Uint8Array)) throw invalid('payload must be binary bytes')
  if (!Number.isInteger(metadata.width) || !Number.isInteger(metadata.height)
      || metadata.width <= 0 || metadata.height <= 0 || metadata.width > 65535 || metadata.height > 65535) throw invalid('invalid image dimensions')
  if (bytes.length < 4 || bytes[0] !== 0xff || bytes[1] !== 0xd8) throw invalid('missing SOI')
  const quant = [-1, -1, -1, -1], huffman = new Uint8Array(8), components = []
  let offset = 2, frame = null, scans = 0, covered = 0, restartInterval = 0
  const word = index => (bytes[index] << 8) | bytes[index + 1]
  while (offset < bytes.length) {
    if (bytes[offset++] !== 0xff) throw invalid('expected a marker')
    while (offset < bytes.length && bytes[offset] === 0xff) offset++
    if (offset >= bytes.length) throw invalid('truncated marker')
    const marker = bytes[offset++]
    if (marker === 0xd9) {
      if (!frame || !scans || covered !== (1 << components.length) - 1) throw invalid('incomplete frame or scans')
      if (offset !== bytes.length) throw invalid('bytes follow EOI')
      return { width: frame.width, height: frame.height }
    }
    if (marker === 0 || marker === 0xd8 || marker >= 0xd0 && marker <= 0xd7) throw invalid('unexpected standalone marker')
    if (offset + 2 > bytes.length) throw invalid('truncated segment length')
    const length = word(offset), begin = offset + 2, end = offset + length
    if (length < 2 || end > bytes.length) throw invalid('segment exceeds image bounds')
    offset = end

    if (marker >= 0xe0 && marker <= 0xef || marker === 0xfe) continue
    if (marker === 0xdb) {
      if (begin === end) throw invalid('empty quantization segment')
      let cursor = begin
      while (cursor < end) {
        const header = bytes[cursor++], precision = header >> 4, table = header & 15
        if (precision > 1 || table > 3 || cursor + 64 * (precision + 1) > end) throw invalid('invalid quantization table')
        for (let item = 0; item < 64; item++) {
          const value = precision ? word(cursor) : bytes[cursor]
          if (!value) throw invalid('zero quantization value')
          cursor += precision + 1
        }
        quant[table] = precision
      }
      continue
    }
    if (marker === 0xc4) {
      if (begin === end) throw invalid('empty Huffman segment')
      let cursor = begin
      while (cursor < end) {
        if (cursor + 17 > end) throw invalid('truncated Huffman table')
        const header = bytes[cursor++], kind = header >> 4, table = header & 15
        if (kind > 1 || table > 3) throw invalid('invalid Huffman table selector')
        let symbols = 0, slots = 1
        for (let size = 0; size < 16; size++) {
          const count = bytes[cursor++]
          symbols += count
          slots = slots * 2 - count
          if (slots < 0) throw invalid('oversubscribed Huffman table')
        }
        if (!symbols || symbols > 256 || slots === 0 || cursor + symbols > end) throw invalid('invalid Huffman symbol bounds')
        cursor += symbols
        huffman[kind * 4 + table] = 1
      }
      continue
    }
    if (marker === 0xdd) {
      if (length !== 4) throw invalid('invalid restart interval')
      restartInterval = word(begin)
      continue
    }
    if (marker >= 0xc0 && marker <= 0xc2) {
      if (frame || scans || length < 11) throw invalid('invalid or repeated SOF')
      const count = bytes[begin + 5], width = word(begin + 3), height = word(begin + 1)
      if (bytes[begin] !== 8 || !count || count > 4 || length !== 8 + 3 * count) throw invalid('unsupported frame layout')
      if (width !== metadata.width || height !== metadata.height) throw invalid('SOF dimensions do not match metadata')
      frame = { marker, width, height }
      for (let index = 0; index < count; index++) {
        const cursor = begin + 6 + index * 3, id = bytes[cursor], sampling = bytes[cursor + 1], table = bytes[cursor + 2]
        if (components.some(component => component.id === id) || !(sampling >> 4) || sampling >> 4 > 4
            || !(sampling & 15) || (sampling & 15) > 4 || table > 3) throw invalid('invalid frame component')
        components.push({ id, table })
      }
      continue
    }
    if (marker !== 0xda) throw invalid('unsupported coding marker')
    if (!frame || length < 8) throw invalid('SOS precedes a complete frame')
    const count = bytes[begin]
    if (!count || count > components.length || length !== 6 + 2 * count) throw invalid('invalid scan component bounds')
    const spectralStart = bytes[end - 3], spectralEnd = bytes[end - 2], high = bytes[end - 1] >> 4, low = bytes[end - 1] & 15
    const progressive = frame.marker === 0xc2
    if (!progressive && (spectralStart !== 0 || spectralEnd !== 63 || high || low)) throw invalid('invalid sequential scan parameters')
    if (progressive && (spectralStart > spectralEnd || spectralEnd > 63 || (spectralStart === 0 ? spectralEnd !== 0 : count !== 1)
        || high > 13 || low > 13 || high !== 0 && high !== low + 1)) throw invalid('invalid progressive scan parameters')
    let selected = 0
    for (let index = 0; index < count; index++) {
      const cursor = begin + 1 + index * 2, component = components.findIndex(value => value.id === bytes[cursor])
      const dc = bytes[cursor + 1] >> 4, ac = bytes[cursor + 1] & 15
      if (component < 0 || selected & (1 << component) || dc > 3 || ac > 3) throw invalid('invalid scan component reference')
      selected |= 1 << component
      if (quant[components[component].table] !== 0) throw invalid('missing 8-bit quantization table')
      if ((!progressive || spectralStart === 0 && high === 0) && !huffman[dc]) throw invalid('missing DC Huffman table')
      if ((!progressive || spectralStart > 0) && !huffman[4 + ac]) throw invalid('missing AC Huffman table')
    }
    if (!progressive || spectralStart === 0 && high === 0) covered |= selected
    scans++
    let entropyBytes = 0, restart = 0
    while (offset < bytes.length) {
      if (bytes[offset] !== 0xff) { entropyBytes++; offset++; continue }
      const markerStart = offset++
      while (offset < bytes.length && bytes[offset] === 0xff) offset++
      if (offset >= bytes.length) throw invalid('truncated entropy marker')
      const code = bytes[offset++]
      if (code === 0) {
        if (offset !== markerStart + 2) throw invalid('fill bytes precede stuffed entropy')
        entropyBytes++
        continue
      }
      if (code >= 0xd0 && code <= 0xd7) {
        if (!restartInterval || !entropyBytes || code !== 0xd0 + restart) throw invalid('invalid restart marker sequence')
        restart = (restart + 1) & 7
        entropyBytes = 0
        continue
      }
      if (!entropyBytes) throw invalid('empty entropy scan')
      offset = markerStart
      break
    }
    if (!entropyBytes) throw invalid('empty entropy scan')
  }
  throw invalid('missing final EOI')
}

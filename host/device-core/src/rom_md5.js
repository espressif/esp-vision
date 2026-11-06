// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// MD5 verifies transport and esptool write receipts; approved source bytes still
// require SHA-256. Web Crypto does not offer the legacy MD5 algorithm.
export function romMd5Hex(bytes) {
  return Array.from(romMd5(bytes), byte => byte.toString(16).padStart(2, '0')).join('')
}

export function romMd5(bytes) {
  const input = new Uint8Array(Math.ceil((bytes.length + 9) / 64) * 64)
  input.set(bytes); input[bytes.length] = 0x80
  const view = new DataView(input.buffer)
  view.setUint32(input.length - 8, (bytes.length * 8) >>> 0, true)
  view.setUint32(input.length - 4, Math.floor(bytes.length / 0x20000000), true)
  const rotations = [[7, 12, 17, 22], [5, 9, 14, 20], [4, 11, 16, 23], [6, 10, 15, 21]]
  const state = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
  for (let offset = 0; offset < input.length; offset += 64) {
    let [a, b, c, d] = state
    for (let i = 0; i < 64; i++) {
      const round = i >>> 4
      const f = round === 0 ? (b & c) | (~b & d) : round === 1 ? (d & b) | (~d & c) : round === 2 ? b ^ c ^ d : c ^ (b | ~d)
      const index = round === 0 ? i : round === 1 ? (5 * i + 1) % 16 : round === 2 ? (3 * i + 5) % 16 : (7 * i) % 16
      const value = (a + f + Math.floor(Math.abs(Math.sin(i + 1)) * 0x100000000) + view.getUint32(offset + index * 4, true)) | 0
      const shift = rotations[round][i % 4]
      const next = (b + ((value << shift) | (value >>> (32 - shift)))) | 0
      a = d; d = c; c = b; b = next
    }
    state[0] = (state[0] + a) >>> 0; state[1] = (state[1] + b) >>> 0
    state[2] = (state[2] + c) >>> 0; state[3] = (state[3] + d) >>> 0
  }
  const output = new Uint8Array(16), result = new DataView(output.buffer)
  state.forEach((value, index) => result.setUint32(index * 4, value, true))
  return output
}

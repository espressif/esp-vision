// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
export const MAX_FIRMWARE_BYTES = 256 * 1024 * 1024
export const MAX_MANIFEST_BYTES = 4 * 1024 * 1024
export const OFFICIAL_ROOT = 'https://dl.espressif.com/AE/esp-vision/'
export const SHA256 = /^[a-f0-9]{64}$/
export const SOURCE_COMMIT = /^[a-f0-9]{40}$/

export function firmwareUrl(value, { official = false } = {}) {
  if (typeof value !== 'string') throw new TypeError('Firmware URLs must use HTTPS')
  let url
  try { url = new URL(value) } catch { throw new TypeError('Firmware URLs must use HTTPS') }
  if (url.protocol !== 'https:' || url.username || url.password || url.hash
      || (official && (!url.href.startsWith(OFFICIAL_ROOT) || url.search || /%2f|%5c|%2e/i.test(url.pathname)))) throw new TypeError('Invalid firmware HTTPS URL or official source')
  return url
}

export async function responseBytes(response, { maxBytes, expectedBytes, signal } = {}) {
  const limit = Math.min(maxBytes, expectedBytes ?? maxBytes)
  const advertised = response.headers?.get?.('content-length')
  if (advertised !== null && advertised !== undefined && (!/^\d+$/.test(advertised) || Number(advertised) > limit)) throw new RangeError('Firmware response exceeds approved byte limit')
  let bytes
  if (response.body?.getReader) {
    const reader = response.body.getReader(), chunks = []
    let length = 0
    const abort = () => { void reader.cancel().catch(() => {}) }
    signal?.addEventListener('abort', abort, { once: true })
    try {
      for (;;) {
        signal?.throwIfAborted()
        const { done, value } = await reader.read()
        if (done) break
        length += value.byteLength
        if (length > limit) throw new RangeError('Firmware response exceeds approved byte limit')
        chunks.push(value)
      }
      bytes = new Uint8Array(length)
      let offset = 0
      for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength }
    } catch (error) { await reader.cancel().catch(() => {}); throw error }
    finally { signal?.removeEventListener('abort', abort); reader.releaseLock() }
  } else if (response.arrayBuffer) bytes = new Uint8Array(await response.arrayBuffer())
  else if (response.text) bytes = new TextEncoder().encode(await response.text())
  else throw new TypeError('Firmware response must expose bytes')
  signal?.throwIfAborted()
  if (bytes.byteLength > limit) throw new RangeError('Firmware response exceeds approved byte limit')
  if (expectedBytes !== undefined && bytes.byteLength !== expectedBytes) throw new Error('Firmware response size does not match the approved build')
  return bytes
}

export async function downloadBytes(url, { fetch: fetcher = globalThis.fetch, signal, maxBytes, expectedBytes, official = false, immutable = false } = {}) {
  firmwareUrl(url, { official })
  if (typeof fetcher !== 'function') throw new TypeError('Provide fetch or in-memory firmware bytes')
  signal?.throwIfAborted()
  const request = cache => fetcher(url, { signal, cache, redirect: 'error' })
  let response
  try { response = await request(immutable ? 'default' : 'no-store') }
  catch (error) {
    // An older navigation/download response may be cached without CORS headers.
    // Retry only a fetch-stage TypeError once, never HTTP, body, hash or metadata
    // failures. Every successful response still undergoes the same hash checks.
    if (!immutable || signal?.aborted || error?.name !== 'TypeError') throw error
    response = await request('no-store')
  }
  if (!response.ok) throw Object.assign(new Error(`Firmware download failed: HTTP ${response.status}`), { code: 'FIRMWARE_DOWNLOAD_FAILED' })
  if (response.url) firmwareUrl(response.url, { official })
  return responseBytes(response, { maxBytes, expectedBytes, signal })
}

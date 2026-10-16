/* SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */
import { loadOfficialFirmware } from './firmware_feed.js'
import { FirmwareFlashSession, validateFirmwareImage, firmwareMode } from './firmware_flash.js'

function record(value, keys, name) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new TypeError(`${name} must be an object`)
  for (const key of Object.keys(value)) if (!keys.includes(key)) throw new TypeError(`Unknown ${name} field: ${key}`)
}
function abort(signal) {
  if (signal !== undefined && (!signal || typeof signal.addEventListener !== 'function' || typeof signal.aborted !== 'boolean')) throw new TypeError('signal must be an AbortSignal')
  if (signal?.aborted) throw signal.reason instanceof Error ? signal.reason : Object.assign(new Error('Firmware operation cancelled'), { name: 'AbortError' })
}
function identity(value) {
  if (!value || typeof value.board !== 'string' || !value.board || typeof value.firmwareVersion !== 'string' || !value.firmwareVersion) throw new TypeError('An inspected identity with board and firmwareVersion is required')
  return structuredClone(value)
}
function release(value, mode = 'update') {
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
  return structuredClone(value)
}
async function download(url, fetcher, signal) {
  if (typeof url !== 'string' || new URL(url).protocol !== 'https:') throw new TypeError('Firmware URLs must use HTTPS')
  if (typeof fetcher !== 'function') throw new TypeError('Provide fetch or an in-memory catalog/image')
  abort(signal)
  const response = await fetcher(url, { signal, cache: 'no-store' })
  if (!response.ok) throw Object.assign(new Error(`Firmware download failed: HTTP ${response.status}`), { code: 'FIRMWARE_DOWNLOAD_FAILED' })
  return response
}

export async function checkFirmware(request = {}, connectedIdentity) {
  record(request, ['catalog', 'identity', 'version', 'mode', 'fetch', 'signal'], 'firmware.check')
  const { signal, fetch: fetcher = globalThis.fetch } = request
  abort(signal)
  if (request.version !== undefined && (typeof request.version !== 'string' || !request.version.trim())) throw new TypeError('version must be a non-empty firmware version string')
  const mode = firmwareMode(request.mode)
  const current = identity(connectedIdentity ?? request.identity)
  const catalog = request.catalog === undefined ? await loadOfficialFirmware({ board: current.board, fetch: fetcher, signal }) : typeof request.catalog === 'string' ? await (await download(request.catalog, fetcher, signal)).json() : structuredClone(request.catalog)
  abort(signal)
  if (catalog?.schemaVersion !== 1 || !Array.isArray(catalog.releases)) throw new TypeError('Catalog requires schemaVersion 1 and releases ordered newest first')
  // Feed ordering is authoritative: firmware versions may be calendar/git labels.
  const candidates = catalog.releases.filter(item => item?.board === current.board).map(item => {
    if (item.compatible !== false) return release(item)
    if (typeof item.version !== 'string' || !item.version) throw new TypeError('Historical release requires a version')
    return structuredClone(item)
  })
  if (new Set(candidates.map(item => item.version)).size !== candidates.length) throw new TypeError('Catalog must not contain duplicate versions for one board')
  const selected = request.version === undefined ? candidates.find(item => item.compatible !== false && (mode !== 'install' || item.install)) ?? null : candidates.find(item => item.version === request.version)
  if (request.version !== undefined && !selected) throw Object.assign(new Error('Requested firmware version is unavailable for this board'), { code: 'FIRMWARE_VERSION_UNAVAILABLE' })
  if (selected?.compatible === false) throw Object.assign(new Error('This published version lacks approved v4 update metadata'), { code: 'FIRMWARE_INCOMPATIBLE' })
  if (selected) release(selected, mode)
  return { available: selected !== null && (selected.firmwareVersion ?? selected.version) !== current.firmwareVersion, currentVersion: current.firmwareVersion, release: selected, releases: candidates }
}

export async function updateFirmware(request, connectedIdentity, disconnect) {
  record(request, ['release', 'board', 'mode', 'data', 'fetch', 'openLoader', 'reset', 'onProgress', 'onPhase', 'signal'], 'firmware.update')
  const { signal, fetch: fetcher = globalThis.fetch, openLoader, reset, onProgress = () => {}, onPhase = () => {} } = request
  abort(signal)
  if (typeof openLoader !== 'function' || typeof onProgress !== 'function' || typeof onPhase !== 'function' || (reset !== undefined && typeof reset !== 'function')) throw new TypeError('Firmware update requires openLoader and valid callbacks')
  const mode = firmwareMode(request.mode)
  const target = release(request.release, mode)
  const image = mode === 'install' ? {
    ...target.image, offset: target.install.offset, bytes: target.install.bytes,
    flashSize: target.install.flashSize, sha256: target.install.sha256, url: target.install.url,
    bootloader: target.install.bootloader, application: target.image,
    chipRevision: undefined, chipRevisionMin: target.install.chipRevisionMin, chipRevisionMax: target.install.chipRevisionMax,
  } : target.image
  const board = connectedIdentity?.board ?? request.board
  if (!board || board !== target.board || (request.board !== undefined && request.board !== board)) throw new TypeError('Firmware release does not match the connected or explicitly selected board')
  const bytes = request.data === undefined ? new Uint8Array(await (await download(image.url, fetcher, signal)).arrayBuffer()) : request.data instanceof Uint8Array ? request.data.slice() : request.data
  await validateFirmwareImage(image, bytes, { mode })
  abort(signal)
  // Validate everything before releasing EV-MUX or acquiring a download port.
  await disconnect()
  abort(signal)
  const loader = await openLoader({ signal })
  let session
  try {
    abort(signal)
    session = new FirmwareFlashSession(loader, image, { mode, reset: reset ? () => reset(loader) : undefined })
    await session.connect(bytes)
    abort(signal)
    // No interruption after writing starts: finish verification and reset policy.
    const result = await session.flash(bytes, { onProgress, onPhase })
    return { ...result, version: target.version, board: target.board, reconnected: false }
  } finally {
    if (session) await session.disconnect()
    else await loader.disconnect()
  }
}

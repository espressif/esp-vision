// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { firmwareSha256 } from './firmware_flash.js'
import { validateFirmwareRelease } from './firmware_release.js'
import { OFFICIAL_ROOT as base, MAX_MANIFEST_BYTES, MAX_FIRMWARE_BYTES, SHA256, SOURCE_COMMIT, firmwareUrl, downloadBytes } from './firmware_io.js'
export async function loadOfficialFirmware({ board, version, fetch: fetcher = globalThis.fetch, signal } = {}) {
  if (board !== undefined && !validName(board, 96)) throw new TypeError('board must be a non-empty board string')
  if (version !== undefined && !validName(version)) throw new TypeError('version must be a non-empty firmware version string')
  const { parse } = await import('smol-toml')
  const get = async (url, optional = false) => {
    signal?.throwIfAborted()
    const response = await fetcher(url, { signal, cache: 'no-store' })
    if (optional && response.status === 404) return null
    if (!response.ok) throw Object.assign(new Error(`Official firmware request failed: HTTP ${response.status}`), { code: 'FIRMWARE_DOWNLOAD_FAILED' })
    return response
  }
  const config = parse(await (await get(base + 'config.toml')).text())
  if (Object.hasOwn(config, 'publication')) {
    const publication = validatePublication(config.publication)
    const selected = publication.releases.find(entry => entry.version === (version ?? publication.version))
    if (!selected || (board && !selected.boards.includes(board))) throw unavailable()
    const manifest = await loadReleaseManifest({
      url: selected.manifestUrl, sha256: selected.manifestSha256,
      version: selected.version, boards: selected.boards, sourceCommit: selected.version === publication.version ? publication.sourceCommit : undefined,
      fetch: fetcher, signal,
    })
    return { ...manifest, releases: manifest.releases.filter(entry => !board || entry.board === board), publication }
  }
  const app = config['esp-vision'], web = config.website
  if (!app?.image || !Array.isArray(web?.releases) || typeof web.binaryNameTemplate !== 'string') throw new TypeError('Invalid official firmware index')
  const boards = Object.keys(app.image).filter(name => !board || name === board)
  const knownVersions = [...new Set([web.release, ...web.releases].filter(value => typeof value === 'string' && value))]
  if (version !== undefined && !knownVersions.includes(version)) throw unavailable()
  const versions = version === undefined ? knownVersions : [version]
  if (versions.length > 256 || boards.length > 64) throw new RangeError('Official firmware index exceeds limits')
  const releases = []
  // Sequential metadata requests keep the official server load bounded.
  for (const version of versions) for (const name of boards) {
    const metadata = await get(`${base}firmware/${encodeURIComponent(name)}/${encodeURIComponent(version)}.json`, true)
    if (metadata) {
      const entry = await metadata.json()
      if (entry.board !== name || entry.version !== version || new URL(entry.image?.url).origin !== 'https://dl.espressif.com') throw new TypeError('Official firmware metadata identity/URL mismatch')
      if (entry.install && new URL(entry.install.url).origin !== 'https://dl.espressif.com') throw new TypeError('Official install URL mismatch')
      releases.push(entry)
    } else {
      const file = web.binaryNameTemplate.replace('{board}', name).replace('{tag}', version)
      const url = new URL(file, config.firmware_images_url)
      if (url.origin !== 'https://dl.espressif.com') throw new TypeError('Invalid official image URL')
      releases.push({ board: name, version, compatible: false, reason: 'UPDATE_METADATA_UNAVAILABLE', downloadUrl: url.href })
    }
  }
  return { schemaVersion: 1, releases }
}

function validName(value, max = 255) { return typeof value === 'string' && value.length > 0 && value.length <= max && value.trim() === value }
function unavailable() { return Object.assign(new Error('Requested firmware version is unavailable'), { code: 'FIRMWARE_VERSION_UNAVAILABLE' }) }
function descriptor(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)
      || !validName(value.version) || !SHA256.test(value.manifestSha256 ?? '')
      || !Array.isArray(value.boards) || !value.boards.length || value.boards.length > 64
      || value.boards.some(board => !validName(board, 96)) || new Set(value.boards).size !== value.boards.length) throw new TypeError('Invalid publication manifest reference')
  firmwareUrl(value.manifestUrl, { official: true })
}
function validatePublication(value) {
  if (!value || value.schemaVersion !== 1 || !SOURCE_COMMIT.test(value.sourceCommit ?? '')
      || !Array.isArray(value.releases) || !value.releases.length || value.releases.length > 256) throw new TypeError('Invalid official publication index')
  if (!validName(value.version) || !SHA256.test(value.manifestSha256 ?? '')) throw new TypeError('Invalid publication current manifest reference')
  firmwareUrl(value.manifestUrl, { official: true })
  const seen = new Set()
  for (const item of value.releases) {
    descriptor(item)
    if (seen.has(item.version)) throw new TypeError('Duplicate publication version')
    seen.add(item.version)
  }
  const current = value.releases.find(item => item.version === value.version)
  if (!current || current.manifestUrl !== value.manifestUrl || current.manifestSha256 !== value.manifestSha256) throw new TypeError('Publication current manifest reference differs from its history')
  return structuredClone(value)
}
function validateWebsite(website, version, sourceCommit) {
  if (!website || typeof website !== 'object' || Array.isArray(website)
      || typeof website.schemaVersion !== 'string' || !/^\d+\.\d+\.\d+$/.test(website.schemaVersion)
      || website.release !== version || !Array.isArray(website.releases) || website.releases.length > 256
      || !website.releases.includes(version) || website.releases.some(item => !validName(item))
      || typeof website.binaryNameTemplate !== 'string' || website.binaryNameTemplate.length > 255
      || !website.binaryNameTemplate.includes('{board}') || !website.binaryNameTemplate.includes('{tag}')
      || !Array.isArray(website.boards) || website.boards.length > 64
      || !Array.isArray(website.models) || website.models.length > 256) throw new TypeError('Invalid release manifest website snapshot')
  const boards = new Set(), models = new Set()
  for (const board of website.boards) {
    if (!board || !validName(board.board, 96) || !validName(board.slug, 96) || !validName(board.name)
        || typeof board.flashable !== 'boolean' || boards.has(board.board)) throw new TypeError('Invalid or duplicate manifest website board')
    if (board.docsUrl !== undefined) firmwareUrl(board.docsUrl)
    boards.add(board.board)
  }
  for (const model of website.models) {
    if (!model || !validName(model.slug, 96) || models.has(model.path)
        || !Number.isSafeInteger(model.sizeBytes) || model.sizeBytes <= 0 || model.sizeBytes > MAX_FIRMWARE_BYTES
        || typeof model.path !== 'string' || !model.path.startsWith('models/') || model.path.length > 512
        || model.path.split('/').some(part => !part || part === '.' || part === '..')
        || model.downloadUrl !== `https://raw.githubusercontent.com/espressif/esp-vision/${sourceCommit}/${model.path}`) throw new TypeError('Invalid manifest model or unpinned model URL')
    const modelUrl = firmwareUrl(model.downloadUrl)
    if (modelUrl.search || !modelUrl.pathname.startsWith(`/espressif/esp-vision/${sourceCommit}/models/`)
        || /%2f|%5c|%2e/i.test(modelUrl.pathname)) throw new TypeError('Invalid manifest model path or source commit boundary')
    models.add(model.path)
  }
}
function validateManifest(value, { version, sourceCommit, boards } = {}) {
  if (!value || value.schemaVersion !== 1 || !validName(value.version)
      || !['legacy', 'single-bin'].includes(value.layout)
      || !SOURCE_COMMIT.test(value.sourceCommit ?? '') || !Number.isSafeInteger(value.pipelineId) || value.pipelineId <= 0
      || (version !== undefined && value.version !== version) || (sourceCommit !== undefined && value.sourceCommit !== sourceCommit)
      || !Array.isArray(value.releases) || !value.releases.length || value.releases.length > 64) throw new TypeError('Invalid release manifest identity or bounds')
  const seen = new Set()
  for (const entry of value.releases) {
    validateFirmwareRelease(entry)
    if (entry.image.bytes > MAX_FIRMWARE_BYTES || (entry.install && entry.install.bytes > MAX_FIRMWARE_BYTES)) throw new RangeError('Manifest firmware image exceeds byte limit')
    if (entry.version !== value.version || (entry.sourceCommit !== undefined && entry.sourceCommit !== value.sourceCommit) || seen.has(entry.board)) throw new TypeError('Release manifest board/version mismatch or duplicate board')
    seen.add(entry.board)
    if ((value.layout === 'single-bin') !== (entry.image.source !== undefined)) throw new TypeError('Release manifest BIN layout/source mismatch')
    firmwareUrl(entry.image.url, { official: true })
    if (entry.image.source) firmwareUrl(entry.image.source.url, { official: true })
    if (entry.install) { validateFirmwareRelease(entry, 'install'); firmwareUrl(entry.install.url, { official: true }) }
  }
  if (boards !== undefined && (boards.length !== seen.size || boards.some(board => !seen.has(board)))) throw new TypeError('Release manifest board set differs from publication reference')
  validateWebsite(value.website, value.version, value.sourceCommit)
  if (value.website.boards.some(board => board.flashable && !seen.has(board.board))) throw new TypeError('Manifest website advertises an unqualified board')
  if (value.mcp !== undefined && value.mcp !== null) {
    const mcp = value.mcp
    if (!mcp || typeof mcp !== 'object' || Array.isArray(mcp)
        || !Number.isSafeInteger(mcp.bytes) || mcp.bytes <= 0 || mcp.bytes > 16 * 1024 * 1024
        || !SHA256.test(mcp.sha256 ?? '') || mcp.version !== value.version || mcp.sourceCommit !== value.sourceCommit) throw new TypeError('Invalid release manifest MCP descriptor')
    firmwareUrl(mcp.url, { official: true })
    const mcpPath = new URL(mcp.url).pathname
    if (!mcpPath.startsWith('/AE/esp-vision/bin/mcp/') || decodeURIComponent(mcpPath.slice(mcpPath.lastIndexOf('/') + 1)) !== `mcp-pack-${value.version}.json`) throw new TypeError('Invalid release manifest MCP source')
  }
  return value
}

/** Read exactly one immutable official release snapshot, verified over raw bytes. */
export async function loadReleaseManifest({ url, sha256, version, sourceCommit, boards, fetch: fetcher = globalThis.fetch, signal } = {}) {
  if (!SHA256.test(sha256 ?? '') || (version !== undefined && !validName(version))
      || (sourceCommit !== undefined && !SOURCE_COMMIT.test(sourceCommit))
      || (boards !== undefined && (!Array.isArray(boards) || !boards.length || boards.length > 64 || boards.some(board => !validName(board, 96)) || new Set(boards).size !== boards.length))) throw new TypeError('Manifest requires a qualified SHA-256, version and source commit')
  const bytes = await downloadBytes(url, { fetch: fetcher, signal, maxBytes: MAX_MANIFEST_BYTES, official: true, immutable: true })
  if (await firmwareSha256(bytes) !== sha256) throw new Error('Release manifest SHA-256 does not match publication')
  signal?.throwIfAborted()
  const value = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes))
  return validateManifest(value, { version, sourceCommit, boards })
}

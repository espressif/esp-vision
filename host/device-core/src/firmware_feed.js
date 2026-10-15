// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
const base = 'https://dl.espressif.com/AE/esp-vision/'
export async function loadOfficialFirmware({ board, fetch: fetcher = globalThis.fetch, signal } = {}) {
  const { parse } = await import('smol-toml')
  const get = async (url, optional = false) => {
    signal?.throwIfAborted()
    const response = await fetcher(url, { signal, cache: 'no-store' })
    if (optional && response.status === 404) return null
    if (!response.ok) throw Object.assign(new Error(`Official firmware request failed: HTTP ${response.status}`), { code: 'FIRMWARE_DOWNLOAD_FAILED' })
    return response
  }
  const config = parse(await (await get(base + 'config.toml')).text())
  const app = config['esp-vision'], web = config.website
  if (!app?.image || !Array.isArray(web?.releases) || typeof web.binaryNameTemplate !== 'string') throw new TypeError('Invalid official firmware index')
  const boards = Object.keys(app.image).filter(name => !board || name === board)
  const versions = [...new Set([web.release, ...web.releases].filter(value => typeof value === 'string' && value))]
  if (versions.length > 256 || boards.length > 64) throw new RangeError('Official firmware index exceeds limits')
  const releases = []
  // Sequential metadata requests keep the official server load bounded.
  for (const version of versions) for (const name of boards) {
    const metadata = await get(`${base}firmware/${encodeURIComponent(name)}/${encodeURIComponent(version)}.json`, true)
    if (metadata) {
      const entry = await metadata.json()
      if (entry.board !== name || entry.version !== version || new URL(entry.image?.url).origin !== 'https://dl.espressif.com') throw new TypeError('Official firmware metadata identity/URL mismatch')
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

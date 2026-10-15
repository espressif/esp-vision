// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
export const ESP_VISION_DISCOVERY_SERVICE = '_esp-vision._tcp.local'

export function normalizeDiscoveredDevice(value) {
  if (!value || !/^[0-9a-f]{12}$/.test(value.deviceId)) throw new Error('Invalid discovered device identity')
  if (typeof value.hostname !== 'string' || value.hostname.length > 63
      || !/^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/.test(value.hostname)) throw new Error('Invalid discovered hostname')
  if (typeof value.board !== 'string' || !value.board || value.board.length > 96
      || typeof value.firmwareVersion !== 'string' || !value.firmwareVersion || value.firmwareVersion.length > 255
      || !Number.isInteger(value.evMuxVersion) || !Number.isInteger(value.port) || value.port < 1 || value.port > 65535) throw new Error('Invalid discovery metadata')
  const addresses = [...new Set(value.addresses || [])].filter(address => typeof address === 'string'
    && address.split('.').length === 4 && address.split('.').every(part => /^(0|[1-9][0-9]{0,2})$/.test(part) && Number(part) <= 255))
  if (!addresses.length) throw new Error('No discovered IPv4 address')
  return { deviceId: value.deviceId, hostname: value.hostname, board: value.board,
    firmwareVersion: value.firmwareVersion, evMuxVersion: value.evMuxVersion,
    compatible: value.evMuxVersion === 4, port: value.port, addresses,
    ...(value.connectionUrl ? { connectionUrl: value.connectionUrl } : {}),
    ...(value.expiresAt ? { expiresAt: value.expiresAt } : {}) }
}

export class NetworkDiscovery {
  constructor(provider) {
    if (typeof provider?.list !== 'function') throw new TypeError('Discovery provider.list is required')
    this.provider = provider
  }
  async list({ timeout = 3000, signal } = {}) {
    if (!Number.isInteger(timeout) || timeout < 50 || timeout > 10000) throw new RangeError('Discovery timeout must be 50..10000 ms')
    signal?.throwIfAborted()
    const controller = new AbortController()
    const abort = () => controller.abort(signal.reason)
    signal?.addEventListener('abort', abort, { once: true })
    // Provider collection closes at timeout. A grace budget catches faulty adapters.
    const timer = setTimeout(() => controller.abort(new Error('Discovery provider timed out')), timeout + 1000)
    let onAbort
    try {
      const aborted = new Promise((_, reject) => {
        onAbort = () => reject(controller.signal.reason)
        controller.signal.addEventListener('abort', onAbort, { once: true })
      })
      const devices = await Promise.race([this.provider.list({ timeout, signal: controller.signal }), aborted])
      controller.signal.throwIfAborted()
      if (!Array.isArray(devices) || devices.length > 256) throw new Error('Invalid discovery response')
      return devices.map(normalizeDiscoveredDevice).sort((a, b) => a.hostname.localeCompare(b.hostname))
    } finally {
      clearTimeout(timer)
      signal?.removeEventListener('abort', abort)
      controller.signal.removeEventListener('abort', onAbort)
      controller.abort()
    }
  }
}

export function createBridgeDiscovery(connectionUrl, { fetch: fetcher = globalThis.fetch } = {}) {
  const endpoint = new URL(connectionUrl)
  if (!['ws:', 'wss:'].includes(endpoint.protocol)) throw new Error('A ws/wss bridge address is required')
  endpoint.protocol = endpoint.protocol === 'wss:' ? 'https:' : 'http:'
  endpoint.pathname = '/devices'
  endpoint.search = ''
  endpoint.hash = ''
  endpoint.username = ''; endpoint.password = ''
  return new NetworkDiscovery({ async list({ timeout, signal }) {
    endpoint.searchParams.set('timeout', String(timeout))
    const response = await fetcher(endpoint, { signal, credentials: 'omit', cache: 'no-store' })
    if (!response.ok) throw new Error(`Network discovery failed (${response.status})`)
    const body = await response.json()
    if (!Array.isArray(body.devices)) throw new Error('Invalid bridge discovery response')
    return body.devices.map(device => {
      const target = new URL(device.connectionUrl, connectionUrl)
      if (target.origin !== new URL(connectionUrl).origin || !/^\/device\/[a-f0-9]{32}$/.test(target.pathname)
          || target.search || target.hash || target.username || target.password) throw new Error('Invalid discovered connection URL')
      return { ...device, connectionUrl: target.href }
    })
  } })
}

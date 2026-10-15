// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
// Optional Node adapter: the browser-safe root has no Node or mDNS dependency.
import { NetworkDiscovery, ESP_VISION_DISCOVERY_SERVICE, normalizeDiscoveredDevice } from './network_discovery.js'
const dnsName = value => String(value).toLowerCase().replace(/\.$/, '')
const privateAddress = address => /^(10\.|192\.168\.|169\.254\.)/.test(address)
  || /^172\.(1[6-9]|2[0-9]|3[01])\./.test(address)

export function createNodeDiscovery({ createMdns, interfaceAddress } = {}) {
  return new NetworkDiscovery({ async list({ timeout, signal }) {
    signal.throwIfAborted()
    const factory = createMdns || (await import('multicast-dns')).default
    signal.throwIfAborted()
    const mdns = factory({ ...(interfaceAddress ? { interface: interfaceAddress } : {}), bind: '0.0.0.0', reuseAddr: true })
    return new Promise((resolve, reject) => {
      const records = new Map()
      let timer, retry, receivedSequence = 0, finished = false
      const finish = (error) => {
        if (finished) return
        finished = true
        clearTimeout(timer); clearInterval(retry)
        signal.removeEventListener('abort', abort)
        mdns.removeListener('response', response)
        mdns.destroy()
        if (error) reject(error)
        else {
          try { resolve(snapshot()) } catch (failure) { reject(failure) }
        }
      }
      const abort = () => finish(signal.reason)
      const values = (name, type) => [...records.values()]
        .filter(r => r.name === name && r.type === type && r.expiresAt > Date.now())
        .sort((a, b) => b.sequence - a.sequence)
      const snapshot = () => {
        const devices = new Map()
        for (const ptr of values(ESP_VISION_DISCOVERY_SERVICE, 'PTR')) {
          const instance = dnsName(ptr.data)
          const srv = values(instance, 'SRV')[0], txt = values(instance, 'TXT')[0]
          if (!srv || !txt) continue
          if (!Array.isArray(txt.data) || !txt.data.every(item => item instanceof Uint8Array)) continue
          const metadata = Object.create(null)
          let valid = true, size = 0
          for (const item of txt.data || []) {
            const entry = new TextDecoder().decode(item); size += item.length + 1
            const split = entry.indexOf('='); const key = entry.slice(0, split)
            if (split < 1 || key in metadata) { valid = false; break }
            metadata[key] = entry.slice(split + 1)
          }
          if (!valid || size > 256 || metadata.txtvers !== '1') continue
          if (dnsName(srv.data.target) !== dnsName(metadata.hostname + '.local')) continue
          const addresses = values(dnsName(srv.data.target), 'A').filter(r => privateAddress(r.data))
          try {
            const candidate = normalizeDiscoveredDevice({ deviceId: metadata.id, hostname: metadata.hostname,
              board: metadata.board, firmwareVersion: metadata.fw, evMuxVersion: Number(metadata.evmux),
              port: srv.data.port, addresses: addresses.map(r => r.data),
              expiresAt: Math.min(ptr.expiresAt, srv.expiresAt, txt.expiresAt, ...addresses.map(r => r.expiresAt)) })
            const key = candidate.deviceId + ':' + candidate.port
            const previous = devices.get(key)
            if (previous && previous.hostname === candidate.hostname) {
              previous.addresses = [...new Set([...previous.addresses, ...candidate.addresses])]
              previous.expiresAt = Math.min(previous.expiresAt, candidate.expiresAt)
            } else if (!previous) devices.set(key, candidate)
          } catch { /* incomplete or invalid advertisements are not candidates */ }
        }
        return [...devices.values()]
      }
      const query = () => {
        const questions = [{ name: ESP_VISION_DISCOVERY_SERVICE, type: 'PTR' }]
        for (const ptr of values(ESP_VISION_DISCOVERY_SERVICE, 'PTR')) {
          const instance = dnsName(ptr.data)
          for (const type of ['SRV', 'TXT']) if (!values(instance, type).length) questions.push({ name: instance, type })
          for (const srv of values(instance, 'SRV')) if (!values(dnsName(srv.data.target), 'A').length) questions.push({ name: srv.data.target, type: 'A' })
        }
        try { mdns.query({ questions: questions.slice(0, 64) }) } catch (error) { finish(error) }
      }
      const response = packet => {
        const now = Date.now()
        const sequence = ++receivedSequence
        for (const [key, cached] of records) if (cached.expiresAt <= now) records.delete(key)
        for (const r of [...(packet.answers || []), ...(packet.additionals || [])].slice(0, 256)) {
          if (!['PTR', 'SRV', 'TXT', 'A'].includes(r.type) || !Number.isFinite(r.ttl) || r.ttl < 0) continue
          if (r.type === 'SRV' && (typeof r.data?.target !== 'string' || !Number.isInteger(r.data.port)
              || r.data.port < 1 || r.data.port > 65535)) continue
          if (['PTR', 'A'].includes(r.type) && typeof r.data !== 'string') continue
          if (r.type === 'TXT' && (!Array.isArray(r.data) || !r.data.every(item => item instanceof Uint8Array)
              || r.data.reduce((size, item) => size + item.length + 1, 0) > 256)) continue
          const name = dnsName(r.name)
          const key = name + ':' + r.type + ':' + JSON.stringify(r.data)
          if (r.ttl === 0) {
            const cached = records.get(key)
            if (cached) cached.expiresAt = Math.min(cached.expiresAt, now + 1000)
            continue
          }
          if (r.flush && r.type !== 'PTR') {
            // RFC 6762: allow split RRSet bursts and re-announcements to rescue records.
            for (const cached of records.values()) if (cached.name === name && cached.type === r.type
                && cached.receivedAt < now - 1000) cached.expiresAt = Math.min(cached.expiresAt, now + 1000)
          }
          if (records.size < 2048 || records.has(key)) records.set(key, {
            name, type: r.type,
            // Detach TXT slices from the original UDP buffer; never retain whole packets.
            data: r.type === 'TXT' ? r.data.map(item => new Uint8Array(item)) : r.data,
            sequence, receivedAt: now, expiresAt: now + Math.min(r.ttl, 120) * 1000,
          })
        }
      }
      mdns.on('error', finish)
      mdns.on('response', response)
      signal.addEventListener('abort', abort, { once: true })
      if (signal.aborted) { abort(); return }
      timer = setTimeout(() => finish(), timeout)
      query()
      if (!finished) retry = setInterval(query, 500)
    })
  } })
}

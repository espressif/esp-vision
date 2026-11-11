// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { serialBinding, serialPortInfo, boundedSerial } from './node_serial.js'
import { createNodeDiscovery } from './network_discovery_node.js'
import { createDevice } from './index.js'
import { serialTransport, tcpTransport } from './node_transport.js'

export async function listSerial({ signal, timeout = 10000 } = {}, dependencies = {}) {
  if (!Number.isFinite(timeout) || timeout <= 0) throw new RangeError('Serial enumeration timeout must be positive')
  try {
    return await boundedSerial(async () => {
      const binding = await (dependencies.binding ?? serialBinding)()
      const ports = await binding.list()
      if (!Array.isArray(ports)) throw new Error('Invalid serial list')
      return Promise.all(ports.map(port => serialPortInfo(port, dependencies)))
    }, { signal, timeout })
  } catch (cause) {
    if (signal?.aborted) throw signal.reason
    if (cause?.code === 'TIMEOUT' || cause?.code === 'SERIAL_BACKEND_UNAVAILABLE') throw cause
    throw Object.assign(new Error('Serial enumeration unavailable; check the npm serial backend and OS permissions'), { code: 'SERIAL_UNAVAILABLE', cause })
  }
}
// Filter OS-enumerated ports, never infer device existence from a name alone.
// USB descriptors are not a firmware/board identity and no VID whitelist applies.
export function filterSerialPorts(ports, { platform = process.platform, allPorts = false } = {}) {
  const valid = ports.filter(port => {
    const path = port.path
    if (typeof path !== 'string' || /[\x00-\x1f\x7f]/.test(path)) return false
    if (platform === 'win32') return /^(?:\\\\\.\\)?COM[1-9]\d*$/i.test(path)
    if (platform === 'darwin') return /^\/dev\/(?:cu|tty)\.[^/]+$/.test(path)
    if (platform === 'linux') return /^\/dev\/[^/]+$/.test(path) && !/^\/dev\/(?:tty\d*|console|ptmx)$/.test(path)
    return false
  })
  const paths = new Set(valid.map(port => port.path))
  const seen = new Set()
  return valid.filter(port => {
    const path = port.path
    if (platform === 'darwin' && path.startsWith('/dev/tty.') && paths.has(path.replace('/dev/tty.', '/dev/cu.'))) return false
    const key = platform === 'win32' ? path.replace(/^\\\\\.\\/, '').toUpperCase() : path
    if (seen.has(key)) return false
    seen.add(key)
    const usb = port.vendorId != null || /\bUSB\b/i.test(port.hwid ?? '')
    const usbName = platform === 'linux' ? /^\/dev\/tty(?:ACM|USB|XRUSB)\d+$/.test(path)
      : platform === 'darwin' && /^\/dev\/(?:cu|tty)\.[^/]*usb[^/]*$/i.test(path)
    return allPorts || usb || usbName
  })
}
export function serialEntry(port) {
  const vid = port.vendorId, pid = port.productId
  const usb = vid !== null && vid !== undefined
  const rom = vid === 0x303a && [2, 9, 18, 32].includes(pid)
  const kind = vid === 0x303a && pid === 0x1001 ? 'usj'
    : [0x10c4, 0x1a86, 0x0403, 0x067b].includes(vid) ? 'uart'
    : vid === 0x303a && ([0x4003, 0x4023].includes(pid) || /esp.vision/i.test(port.product ?? '')) ? 'usb'
    : usb ? 'serial' : 'uart'
  return { interface: kind, address: port.path, status: rom ? 'rom' : 'unprobed', identity: null, usb: { vendorId: vid ?? null, productId: pid ?? null, serialNumber: port.serialNumber ?? null, product: port.product ?? null, manufacturer: port.manufacturer ?? null }, interfaceSource: 'descriptor-candidate' }
}
export function transportFor(entry, options = {}) {
  return entry.interface === 'tcp' ? tcpTransport({ host: entry.host, port: entry.port })
    : serialTransport({ serial: entry.address, role: entry.interface })
}
export async function inventory(options = {}, dependencies = {}) {
  const { signal, timeout = 3000, probe = false, interfaces = ['all'] } = options
  const matches = kind => interfaces.includes('all') || interfaces.includes(kind) || (kind !== 'tcp' && interfaces.includes('serial'))
  const warnings = [], rows = []
  let sourcesAvailable = 0
  const tasks = []
  if (interfaces.some(kind => kind !== 'tcp')) tasks.push((async () => {
    try {
      const ports = await (dependencies.listSerial ?? listSerial)({ ...options, signal })
      rows.push(...filterSerialPorts(ports, { platform: dependencies.platform ?? process.platform, allPorts: options.allPorts }).map(serialEntry).filter(row => matches(row.interface)))
      sourcesAvailable++
    }
    catch (error) { if (signal?.aborted) throw error; warnings.push({ source: 'serial', code: error.code ?? 'SERIAL_UNAVAILABLE', message: 'Serial enumeration unavailable; check the npm serial backend and access permissions' }) }
  })())
  if (matches('tcp')) tasks.push((async () => {
    try {
      const devices = await (dependencies.listNetwork ?? (request => createNodeDiscovery().list(request)))({ timeout, signal })
      sourcesAvailable++
      rows.push(...devices.map(device => ({ interface: 'tcp', address: `${device.addresses[0]}:${device.port}`, host: device.addresses[0], port: device.port, status: 'advertised', identity: { deviceId: device.deviceId, hostname: device.hostname, board: device.board, firmwareVersion: device.firmwareVersion }, advertisement: device })))
    } catch (error) { if (signal?.aborted) throw error; warnings.push({ source: 'tcp', code: error.code ?? 'DISCOVERY_UNAVAILABLE', message: `LAN discovery unavailable: ${error.message || 'Unknown discovery error'}` }) }
  })())
  await Promise.all(tasks)
  signal?.throwIfAborted()
  if (probe) for (const row of rows) {
    signal?.throwIfAborted()
    if (row.status === 'rom') {
      try {
        const ports = await (dependencies.listSerial ?? listSerial)({ ...options, signal })
        const selected = ports.filter(port => port.path === row.address)
        if (selected.length !== 1) throw Object.assign(new Error('Selected ROM port is no longer unique'), { code: 'ROM_PORT_LOST' })
        const probeRom = dependencies.probeRom ?? (await import('./node_rom_probe.js')).probeNodeRom
        const result = await probeRom(selected[0], { timeout: options.probeTimeout ?? 5000, signal })
        row.romProbe = result.probe
        if (!result.port || result.probe.outcome !== 'runtime') {
          row.status = result.port ? 'rom' : 'unidentified'
          if (!result.port) row.error = { code: 'ROM_RUNTIME_NOT_FOUND', message: 'Normal boot did not expose a runtime; enter ROM download mode again to install firmware' }
          continue
        }
        Object.assign(row, serialEntry(result.port))
      } catch (error) {
        signal?.throwIfAborted()
        row.romProbe = error.romProbe
        row.error = { code: error.code ?? 'ROM_PROBE_FAILED', message: error.message }
        continue
      }
    }
    const device = (dependencies.createDevice ?? createDevice)({ transport: (dependencies.transportFor ?? transportFor)(row, options) })
    try {
      const result = await device.connect({ role: row.interface === 'serial' ? 'unknown' : row.interface, timeout: options.probeTimeout ?? 5000, signal, probeRepl: false, autoBaud: false, ...(row.advertisement ? { expectedDevice: row.advertisement } : {}) })
      if (row.romProbe?.mac && result.identity?.deviceId !== row.romProbe.mac) throw Object.assign(new Error('Runtime identity differs from the selected ROM device'), { code: 'DEVICE_IDENTITY_MISMATCH' })
      row.identity = result.identity; row.inspection = await device.inspect({ fields: options.fields, signal }); row.status = 'identified'
    } catch (error) {
      if (signal?.aborted) throw error
      row.status = error.code === 'SERIAL_ACCESS_DENIED' ? 'permission-denied' : ['SERIAL_BUSY', 'SESSION_BUSY'].includes(error.code) ? 'busy' : 'unidentified'; row.error = { code: error.code ?? 'PROBE_FAILED', message: 'Probe failed; the port may be busy, inaccessible, unflashed or running another firmware' }
    } finally { await device.disconnect().catch(() => { row.cleanupFailed = true }) }
  }
  const rank = row => row.status === 'identified' ? 0 : row.status === 'advertised' ? 1 : row.status === 'unprobed' ? 2 : 3
  rows.sort((a, b) => rank(a) - rank(b) || a.address.localeCompare(b.address, 'en', { numeric: true }))
  const groups = new Map()
  for (const [index, row] of rows.entries()) {
    row.number = index + 1
    const id = row.identity?.deviceId
    if (id && !groups.has(id)) groups.set(id, groups.size + 1)
    row.group = id ? groups.get(id) : null
  }
  return { devices: rows, warnings, sourcesAvailable }
}

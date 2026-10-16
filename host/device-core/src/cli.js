#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { parseArgs } from 'node:util'
import { readFile } from 'node:fs/promises'
import { realpathSync } from 'node:fs'
import { resolve } from 'node:path'
import { pathToFileURL } from 'node:url'
import { createInterface } from 'node:readline/promises'
import { Writable } from 'node:stream'
import stringWidth from 'string-width'
import { createDevice } from './index.js'
import { openNodeRom } from './node_rom.js'
import { inventory, transportFor, defaultPython } from './node_inventory.js'

export const usage = `esp-vision-device inspect [--port PATH | --tcp HOST[:PORT] | --select N]
  [--fields identity,capabilities,runtime,network,storage] [--probe]
  [--interface all|serial|usb|usj|uart|tcp] [--all-ports] [--json]
esp-vision-device configure (--port PATH | --tcp HOST[:PORT])
  [--ssid SSID [--password-stdin]] [--forget-wifi] [--network-enabled true|false]
esp-vision-device control (--port PATH | --tcp HOST[:PORT])
  --action stop|reset|bootloader [--mode soft|hard]
esp-vision-device firmware check [--port PATH | --tcp HOST[:PORT]]
  [--catalog FILE|HTTPS_URL] [--board BOARD --current-version VERSION] [--version VERSION] [--mode update|install]
esp-vision-device firmware update [--port PATH | --tcp HOST[:PORT]]
  [--catalog FILE|HTTPS_URL] --version VERSION [--image FIRMWARE.bin] [--mode update|install]
  [--board BOARD --current-version VERSION]
Common: --timeout MS (default 3000 enumeration, 30000 command),
  --probe-timeout MS (default 5000 per device), --role usb|usj|uart|unknown,
  --python PYTHON (default python3; Windows python), --json, --help
Default inspect enumerates USB serial ports and LAN advertisements; --all-ports
also includes native UART/Bluetooth ports. No serial
port is opened. --probe and targeted commands acquire a session and can interrupt
Python. Rows with unknown firmware remain visible; numbering is per invocation.
Firmware versions come from dl.espressif.com by default; local catalogs are advanced overrides.
Firmware update uses the built-in serial flasher (Python esptool>=5.3,<6). TCP is not OTA.
Default mode update preserves the existing layout/data; explicit mode install writes
a complete image, replacing data in its written range. Blank boards require --board
and a ROM-capable USJ/UART port, not an application USB-OTG interface.`
const error = (code, message) => Object.assign(new Error(message), { code })
const fields = ['identity', 'capabilities', 'runtime', 'network', 'storage']
export function parseCli(args) {
  const boolean = ['help', 'json', 'all-ports', 'probe', 'forget-wifi', 'password-stdin']
  const string = ['port', 'tcp', 'select', 'fields', 'interface', 'python', 'timeout', 'probe-timeout', 'role', 'ssid', 'network-enabled', 'action', 'mode', 'catalog', 'board', 'current-version', 'version', 'image']
  const { values: v, positionals: p } = parseArgs({ args, allowPositionals: true, options: Object.fromEntries([...boolean.map(key => [key, { type: 'boolean' }]), ...string.map(key => [key, { type: 'string' }])]) })
  if (v.help) return { help: true }
  const command = p[0] ?? 'inspect', action = command === 'firmware' ? p[1] : null
  if (!['inspect', 'configure', 'control', 'firmware'].includes(command) || p.length > (command === 'firmware' ? 2 : 1) || (command === 'firmware' && !['check', 'update'].includes(action))) throw error('ARGUMENT', 'Unknown command; use --help')
  const allowed = { inspect: ['all-ports', 'select', 'fields', 'interface', 'probe', 'probe-timeout'], configure: ['ssid', 'forget-wifi', 'password-stdin', 'network-enabled'], control: ['action', 'mode'], firmware: ['mode', 'catalog', 'board', 'current-version', 'version', 'image'] }[command]
  for (const key of Object.keys(v)) if (!['json', 'port', 'tcp', 'python', 'timeout', 'role', ...allowed].includes(key)) throw error('ARGUMENT', `Option --${key} is not valid for ${command}`)
  if ([v.port, v.tcp, v.select].filter(value => value !== undefined).length > 1) throw error('ARGUMENT', 'Select exactly one device address or number')
  if (v.role && !['usb', 'usj', 'uart', 'unknown'].includes(v.role)) throw error('ARGUMENT', 'Invalid serial role')
  if (v.role && !v.port) throw error('ARGUMENT', '--role requires --port')
  if (v.select !== undefined && (!/^\d+$/.test(v.select) || Number(v.select) < 1)) throw error('ARGUMENT', '--select must be a positive result number')
  const timeout = Number(v.timeout ?? (command === 'inspect' && !v.port && !v.tcp ? 3000 : 30000))
  if (!Number.isSafeInteger(timeout) || timeout < 50 || timeout > 600000) throw error('ARGUMENT', 'Timeout must be 50..600000 ms')
  const interfaces = (v.interface ?? 'all').split(',')
  if (interfaces.some(value => !['all', 'serial', 'usb', 'usj', 'uart', 'tcp'].includes(value))) throw error('ARGUMENT', 'Unknown interface filter')
  const selectedFields = v.fields?.split(',')
  if (selectedFields?.some(value => !fields.includes(value))) throw error('ARGUMENT', 'Unknown inspect field')
  if (selectedFields && !v.port && !v.tcp && !v.probe && !v.select) throw error('ARGUMENT', '--fields requires a target, --select or --probe')
  const probeTimeout = Number(v['probe-timeout'] ?? 5000)
  if (!Number.isSafeInteger(probeTimeout) || probeTimeout < 50 || probeTimeout > 60000) throw error('ARGUMENT', 'Probe timeout must be 50..60000 ms')
  let endpoint
  if (v.tcp) {
    const m = /^(?:\[([0-9a-fA-F:]+)\]|([^\s:/@?#]+))(?::([0-9]+))?$/.exec(v.tcp)
    if (!m || Number(m[3] ?? 3232) < 1 || Number(m[3] ?? 3232) > 65535) throw error('ARGUMENT', 'Expected TCP host[:port] without credentials or URL')
    endpoint = { interface: 'tcp', host: m[1] ?? m[2], port: Number(m[3] ?? 3232), address: v.tcp }
  }
  if (v.port) endpoint = { interface: v.role ?? 'unknown', address: v.port }
  if (command === 'configure' || command === 'control') {
    if (!endpoint) throw error('ARGUMENT', 'Specify --port or --tcp')
  }
  if (command === 'configure') {
    if (v['network-enabled'] !== undefined && !['true', 'false'].includes(v['network-enabled'])) throw error('ARGUMENT', 'network-enabled must be true or false')
    if ((v.ssid && v['forget-wifi']) || (v['password-stdin'] && !v.ssid) || (!v.ssid && !v['forget-wifi'] && v['network-enabled'] === undefined)) throw error('ARGUMENT', 'Specify SSID, forget-wifi or network-enabled')
  }
  if (command === 'control' && (!['stop', 'reset', 'bootloader'].includes(v.action) || (v.action === 'reset' ? !['soft', 'hard'].includes(v.mode) : v.mode !== undefined))) throw error('ARGUMENT', 'Control requires action; reset requires mode soft or hard')
  if (command === 'firmware') {
    if (v.mode !== undefined && !['update', 'install'].includes(v.mode)) throw error('ARGUMENT', 'Firmware mode must be update or install')
    if (v.mode === 'install' && !v.board) throw error('ARGUMENT', 'Firmware installation requires explicit --board')
    if (!endpoint && !v.board) throw error('ARGUMENT', 'Firmware requires a connected device or --board')
    if (action === 'update' && (!v.port || !v.version)) throw error('ARGUMENT', 'Firmware update requires local --port and explicit --version')
    if (action === 'check' && v.image) throw error('ARGUMENT', 'Image applies only to firmware update')
  }
  return { ...v, allPorts: v['all-ports'] ?? false, command, action: command === 'firmware' ? action : v.action, endpoint, timeout, probeTimeout, interfaces, fields: selectedFields, select: v.select === undefined ? undefined : Number(v.select), python: v.python ?? defaultPython }
}
async function passwordFromInput(options) {
  if (options['password-stdin']) {
    let value = ''
    for await (const chunk of process.stdin) { value += chunk; if (value.length > 4096) throw error('ARGUMENT', 'Password input is too long') }
    return value.replace(/\r?\n$/, '')
  }
  if (!process.stdin.isTTY) throw error('ARGUMENT', 'Use --password-stdin for non-interactive Wi-Fi configuration')
  process.stderr.write('Wi-Fi password (hidden; empty for open network): ')
  const output = new Writable({ write(_chunk, _encoding, callback) { callback() } })
  const terminal = createInterface({ input: process.stdin, output, terminal: true })
  terminal.once('SIGINT', () => process.kill(process.pid, 'SIGINT'))
  try { return await terminal.question('', { signal: options.signal }) } finally { terminal.close(); process.stderr.write('\n') }
}
export async function runCli(options, dependencies = {}) {
  if (options.help) return { help: usage }
  const signal = dependencies.signal
  const makeDevice = dependencies.createDevice ?? createDevice
  const scan = dependencies.inventory ?? inventory
  let endpoint = options.endpoint
  if (options.command === 'inspect' && !endpoint) {
    const listing = await scan({ ...options, signal, timeout: Math.min(options.timeout, 10000) }, dependencies)
    if (!options.select) {
      if (listing.sourcesAvailable === 0) throw error('ENUMERATION_UNAVAILABLE', 'No inventory source succeeded; check Python/pyserial, multicast-dns and permissions')
      return listing
    }
    endpoint = listing.devices.find(row => row.number === options.select)
    if (!endpoint) throw error('DEVICE_NOT_FOUND', 'Selected result number is unavailable in this enumeration')
  }
  const offline = options.command === 'firmware' && options.board
  const device = makeDevice(endpoint && !offline ? { transport: (dependencies.transportFor ?? transportFor)(endpoint, options) } : {})
  try {
    if (endpoint && !offline) await device.connect({ role: endpoint.interface === 'serial' ? 'unknown' : endpoint.interface, timeout: options.timeout, signal, ...(endpoint.advertisement ? { expectedDevice: endpoint.advertisement } : {}) })
    if (options.command === 'inspect') return await device.inspect({ fields: options.fields, timeout: options.timeout, signal })
    if (options.command === 'configure') {
      const network = {}
      if (options.ssid) network.credentials = { ssid: options.ssid, password: await (dependencies.readPassword ?? passwordFromInput)({ ...options, signal }) }
      if (options['forget-wifi']) network.credentials = null
      if (options['network-enabled'] !== undefined) network.enabled = options['network-enabled'] === 'true'
      await device.configure({ network, timeout: options.timeout, signal })
      return { ok: true } // Do not echo credentials or configuration payloads.
    }
    if (options.command === 'control') return await device.control({ action: options.action, ...(options.mode ? { mode: options.mode } : {}), timeout: options.timeout, signal })
    let catalog = options.catalog
    const mode = options.mode ?? 'update'
    if (catalog !== undefined && !/^https:\/\//.test(catalog)) catalog = JSON.parse(await (dependencies.readFile ?? readFile)(catalog, 'utf8'))
    const checked = await device.firmware.check({ catalog, mode, ...(endpoint && !offline ? {} : { identity: { board: options.board, firmwareVersion: options['current-version'] ?? 'unknown' } }), ...(options.version ? { version: options.version } : {}), signal })
    if (options.action === 'check') return checked
    return await device.firmware.update({ mode, release: checked.release, board: options.board, ...(options.image ? { data: new Uint8Array(await (dependencies.readFile ?? readFile)(options.image)) } : {}), openLoader: ({ signal }) => (dependencies.openLoader ?? openNodeRom)({ port: options.port, python: options.python, signal }), onPhase: dependencies.onPhase ?? (() => {}), signal })
  } finally { await device.disconnect() }
}
const safe = value => String(value ?? '—').replace(/[\x00-\x1f\x7f-\x9f]/g, '?')
const graphemes = new Intl.Segmenter(undefined, { granularity: 'grapheme' })
function wrapCell(value, width) {
  const lines = []; let line = '', used = 0
  for (const { segment } of graphemes.segment(value)) {
    const size = stringWidth(segment)
    if (used + size > width && line) { lines.push(line); line = ''; used = 0 }
    line += segment; used += size
  }
  lines.push(line)
  return lines
}
export function formatResult(result, { columns = process.stdout.columns ?? 120 } = {}) {
  if (!result.devices) return JSON.stringify(result, null, 2)
  const headers = ['#', 'Group', 'Interface', 'Address', 'Status', 'Board', 'Firmware', 'Device ID', 'USB serial', 'Hostname', 'USB VID:PID', 'USB product', 'Error']
  const rows = result.devices.map(row => [row.number, row.group, row.interface, row.address, row.status, row.identity?.board, row.identity?.firmwareVersion, row.identity?.deviceId, row.usb?.serialNumber, row.identity?.hostname, row.usb?.vendorId == null ? null : `${row.usb.vendorId.toString(16).padStart(4, '0')}:${(row.usb.productId ?? 0).toString(16).padStart(4, '0')}`, row.usb?.product, row.error?.code].map(safe))
  if (!rows.length) return 'No devices found.'
  const widths = headers.map((header, index) => Math.max(stringWidth(header), ...rows.map(row => stringWidth(row[index]))))
  if (widths.reduce((sum, width) => sum + width, 0) + 2 * (headers.length - 1) <= columns) {
    return [headers, ...rows].map(row => row.map((value, index) => value + ' '.repeat(widths[index] - stringWidth(value))).join('  ').trimEnd()).join('\n')
  }
  // Keep every field readable when a full table would wrap in a narrow terminal.
  const labelWidth = Math.max(...headers.map(value => stringWidth(value)))
  const indent = ' '.repeat(labelWidth + 2)
  return rows.map(row => headers.map((header, index) => {
    const lines = wrapCell(row[index], Math.max(2, columns - indent.length))
    return `${header.padEnd(labelWidth)}: ${lines.join(`\n${indent}`)}`
  }).join('\n')).join('\n\n')
}

function isMain() {
  try { return Boolean(process.argv[1]) && pathToFileURL(realpathSync(process.argv[1])).href === import.meta.url }
  catch { return false }
}
if (isMain()) {
  const controller = new AbortController()
  const abort = () => controller.abort(error('CANCELLED', 'Command cancelled'))
  process.once('SIGINT', abort); process.once('SIGTERM', abort)
  try {
    const options = parseCli(process.argv.slice(2))
    const result = await runCli(options, { signal: controller.signal, onPhase: message => console.error(message) })
    console.log(options.help ? usage : options.json ? JSON.stringify(result) : formatResult(result))
    if (!options.json) for (const warning of result.warnings ?? []) console.error(`${warning.source}: ${warning.message}`)
  } catch (failure) {
    console.error(JSON.stringify({ ok: false, code: failure.code ?? 'COMMAND_FAILED', message: failure.code === 'ESPTOOL_UNAVAILABLE' || failure.code === 'ARGUMENT' || failure.code?.startsWith('ERR_PARSE_ARGS') ? failure.message : 'Command failed; check device access, firmware compatibility, arguments and adapter dependencies' }))
    process.exitCode = controller.signal.aborted ? 130 : 1
  } finally { process.removeListener('SIGINT', abort); process.removeListener('SIGTERM', abort) }
}

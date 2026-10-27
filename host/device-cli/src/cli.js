#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { parseArgs } from 'node:util'
import { readFile, writeFile } from 'node:fs/promises'
import { realpathSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'
import { createInterface } from 'node:readline/promises'
import { Writable } from 'node:stream'
import stringWidth from 'string-width'
import { createDevice } from '@esp-vision/device-core'
import {
  defaultPython, filterSerialPorts, inventory, listSerial, serialEntry, transportFor,
  openNodeRom,
} from '@esp-vision/device-core/node'

const MAX_FILE_OUTPUT = 16 * 1024 * 1024
const DEFAULT_PYTHON_OUTPUT = 16 * 1024
const MAX_PYTHON_OUTPUT = 1024 * 1024
const MAX_BATCH_OPERATIONS = 100
const schemaVersion = 1
const fields = ['identity', 'capabilities', 'runtime', 'network', 'storage']

export const usage = `esp-vision-device inspect [--port PATH | --tcp HOST[:PORT] | --select N]
  [--fields identity,capabilities,runtime,network,storage] [--probe]
  [--interface all|serial|usb|usj|uart|tcp] [--all-ports] [--json]
esp-vision-device configure (--port PATH | --tcp HOST[:PORT])
  [--ssid SSID [--password-stdin]] [--forget-wifi] [--network-enabled true|false]
esp-vision-device control (--port PATH | --tcp HOST[:PORT])
  --action stop|bootloader
esp-vision-device control (--port PATH | --tcp HOST[:PORT]) --action reset --mode soft|hard
esp-vision-device firmware check [--port PATH | --tcp HOST[:PORT]]
  [--catalog FILE|HTTPS_URL] [--board BOARD --current-version VERSION] [--version VERSION] [--mode update|install]
esp-vision-device firmware update [--port PATH | --tcp HOST[:PORT]]
  [--catalog FILE|HTTPS_URL] --version VERSION [--image FIRMWARE.bin] [--mode update|install]
  [--board BOARD --current-version VERSION]
esp-vision-device python (--source CODE | --file LOCAL.py | --path DEVICE.py)
  [--completion wait|start] [--timeout MS] [--max-output-chars N] [--port PATH | --tcp HOST[:PORT]]
esp-vision-device files list [--path DEVICE_DIR] [--recursive]
esp-vision-device files read DEVICE_PATH --output LOCAL_FILE [--max-bytes N]
esp-vision-device files write DEVICE_PATH --file LOCAL_FILE [--no-overwrite]
esp-vision-device files remove DEVICE_PATH [--type file|directory]
esp-vision-device files mkdir DEVICE_DIR [--recursive]
esp-vision-device capture camera|screen --output LOCAL.jpg [--quality N] [--max-bytes N]
esp-vision-device batch PLAN.json [--port PATH | --tcp HOST[:PORT]]
esp-vision-device watch --duration MS [--port PATH | --tcp HOST[:PORT]]
Common: --timeout MS, --python PYTHON (default python3; Windows python), --json, --help
All commands that operate on a device require --port or --tcp, except inspect and offline firmware check.
Python --completion start accepts only --path DEVICE.py. It acknowledges the start, disconnects, and exits; it does not retain a daemon or resumable job.
Python --timeout is an execution deadline for --completion wait only; omit it with --completion start.
Control --mode applies only to --action reset; omit it with stop or bootloader.
While an application is running, use passive inspect/capture/watch or native file uploads. Python and files list/read/remove/mkdir use the REPL and can interrupt it.
Verify a started app with passive captures before stopping it and reading progress files. Start acknowledgement is not application readiness; runtime.scriptRunning is local to the current session.
Batch plans use schemaVersion 1 with an operations array. Operations run sequentially over one connection and stop at the first failure.
In JSON mode stdout contains one schemaVersion 1 JSON envelope. Python output, status, logs, and errors are NDJSON events on stderr.
Binary file reads and JPEG captures always write to --output. Local output is capped at 16 MiB by default and --max-bytes may lower that cap.
Firmware versions come from dl.espressif.com by default; local catalogs are advanced overrides.
Firmware update uses the built-in serial flasher (Python esptool>=5.3,<6). TCP is not OTA.
Default mode update preserves the existing layout/data; explicit mode install writes a complete image, replacing data in its written range. Blank boards require --board and a ROM-capable USJ/UART port, not an application USB-OTG interface.`

export const cliCapabilities = Object.freeze({
  commands: {
    inspect: ['device discovery', 'identity, capability, runtime, network and storage inspection'],
    configure: ['Wi-Fi credentials and network enablement'],
    control: ['stop', 'soft reset', 'hard reset', 'bootloader handoff'],
    firmware: ['check', 'update'],
    python: ['--source CODE', '--file LOCAL.py', '--path DEVICE.py', 'live output, log and state NDJSON events'],
    files: ['list', 'read', 'write', 'remove', 'mkdir'],
    capture: ['camera JPEG', 'screen JPEG'],
    batch: ['sequential schemaVersion 1 JSON plan', 'one connection', 'stop at first failure'],
    watch: ['bounded output, log, state and diagnostic stream', 'one connection', 'no script launch or reconnect'],
  },
  output: {
    json: 'One schemaVersion 1 envelope on stdout',
    events: 'NDJSON on stderr',
    binary: 'Local files only; never embedded in JSON',
    pythonOutput: 'Live python.output NDJSON on stderr, capped at maxOutputChars (default 16384); one truncated marker when capped',
    pythonLogAndState: 'Live python.log and python.state NDJSON on stderr during Python execution',
  },
  limits: { localFileBytes: MAX_FILE_OUTPUT, pythonOutputCharsDefault: DEFAULT_PYTHON_OUTPUT, pythonOutputCharsMax: MAX_PYTHON_OUTPUT, batchOperations: MAX_BATCH_OPERATIONS, watchDurationMs: { min: 1, max: 3600000 } },
  exitCodes: { success: 0, failure: 1, argumentOrPlan: 2, sigint: 130, sigterm: 143 },
})

const error = (code, message, options = {}) => Object.assign(new Error(message), { code, ...options })
const optionError = message => error('ARGUMENT', message)
const knownDeviceCommands = ['configure', 'control', 'firmware', 'python', 'files', 'capture', 'batch', 'watch']
const commonOptions = ['port', 'tcp', 'python', 'timeout', 'json', 'role']

function devicePath(value, { root = false, fail = optionError } = {}) {
  if (typeof value !== 'string' || !value.startsWith('/') || (!root && value === '/')
      || new TextEncoder().encode(value).length > 123 || /[\x00-\x1f\x7f\\"]/.test(value)
      || value.includes('..') || value.includes('//') || (value.length > 1 && value.endsWith('/'))) {
    const shellHint = typeof value === 'string' && /^[A-Za-z]:[\\/]/.test(value)
      ? ' Received a Windows host path; Git Bash/MSYS may have converted the device path. Use PowerShell or prefix this command with MSYS2_ARG_CONV_EXCL=\"*\" and keep local paths in Windows form.' : ''
    throw fail('Expected an absolute device path (for example /models), at most 123 UTF-8 bytes, without traversal, trailing slash or forbidden characters.' + shellHint)
  }
  return value
}

function parseTcpAddress(value) {
  const match = /^(?:\[([0-9a-fA-F:]+)\]|([^\s:/@?#]+))(?::([0-9]+))?$/.exec(value)
  if (!match || Number(match[3] ?? 3232) < 1 || Number(match[3] ?? 3232) > 65535) {
    throw optionError('Expected TCP host[:port] without credentials or URL')
  }
  return { interface: 'tcp', host: match[1] ?? match[2], port: Number(match[3] ?? 3232), address: value }
}

function parsePositiveInteger(value, label, { max = Number.MAX_SAFE_INTEGER } = {}) {
  if (!/^\d+$/.test(String(value))) throw optionError(`${label} must be a positive integer`)
  const number = Number(value)
  if (!Number.isSafeInteger(number) || number < 1 || number > max) throw optionError(`${label} must be 1..${max}`)
  return number
}

function parseCommandOptions(args, strict = true) {
  const booleans = [
    'help', 'json', 'all-ports', 'probe', 'forget-wifi', 'password-stdin',
    'recursive', 'no-overwrite',
  ]
  const strings = [
    'port', 'tcp', 'select', 'fields', 'interface', 'python', 'timeout', 'probe-timeout',
    'role', 'ssid', 'network-enabled', 'action', 'mode', 'catalog', 'board',
    'current-version', 'version', 'image', 'source', 'file', 'path', 'completion',
    'max-output-chars', 'output', 'max-bytes', 'quality', 'type', 'duration',
  ]
  try {
    return parseArgs({
      args,
      strict,
      allowPositionals: true,
      options: Object.fromEntries([
        ...booleans.map(key => [key, { type: 'boolean' }]),
        ...strings.map(key => [key, { type: 'string' }]),
      ]),
    })
  } catch (cause) {
    throw error('ARGUMENT', cause.message, { cause })
  }
}

export function parseCli(args) {
  const { values: v, positionals: p } = parseCommandOptions(args)
  if (v.help) return { help: true, json: v.json ?? false }

  const command = p[0] ?? 'inspect'
  const subcommand = command === 'firmware' || command === 'files' ? p[1] : null
  const allowedCommands = ['inspect', ...knownDeviceCommands]
  if (!allowedCommands.includes(command)) throw optionError('Unknown command; use --help')

  const expectedPositionals = {
    inspect: 1,
    configure: 1,
    control: 1,
    firmware: 2,
    python: 1,
    files: 3,
    capture: 2,
    batch: 2,
    watch: 1,
  }[command]
  if (p.length > expectedPositionals) throw optionError('Unexpected positional argument')
  if ((command === 'firmware' && !['check', 'update'].includes(subcommand))
      || (command === 'files' && !['list', 'read', 'write', 'remove', 'mkdir'].includes(subcommand))) {
    throw optionError(`Unknown ${command} operation; use --help`)
  }

  const allowed = {
    inspect: ['all-ports', 'select', 'fields', 'interface', 'probe', 'probe-timeout'],
    configure: ['ssid', 'forget-wifi', 'password-stdin', 'network-enabled'],
    control: ['action', 'mode'],
    firmware: ['mode', 'catalog', 'board', 'current-version', 'version', 'image'],
    python: ['source', 'file', 'path', 'completion', 'max-output-chars'],
    files: subcommand === 'list' ? ['path', 'recursive']
      : subcommand === 'read' ? ['output', 'max-bytes']
        : subcommand === 'write' ? ['file', 'no-overwrite']
          : subcommand === 'remove' ? ['type'] : ['recursive'],
    capture: ['output', 'quality', 'max-bytes'],
    batch: [],
    watch: ['duration'],
  }[command]
  for (const key of Object.keys(v)) {
    if (!['help', ...commonOptions, ...allowed].includes(key)) {
      throw optionError(`Option --${key} is not valid for ${command}${subcommand ? ` ${subcommand}` : ''}`)
    }
  }

  const endpointCount = [v.port, v.tcp].filter(value => value !== undefined).length
  if (endpointCount > 1) throw optionError('Select exactly one device address')
  if (v.role && !['usb', 'usj', 'uart', 'unknown'].includes(v.role)) throw optionError('Invalid serial role')
  if (v.role && !v.port) throw optionError('--role requires --port')
  const endpoint = v.tcp ? parseTcpAddress(v.tcp)
    : v.port ? { interface: v.role ?? 'unknown', address: v.port } : undefined

  const explicitTimeout = v.timeout !== undefined
  const timeout = explicitTimeout
    ? parsePositiveInteger(v.timeout, 'Timeout', { max: 600000 })
    : command === 'inspect' && !endpoint && !v.select ? 3000 : 30000
  const probeTimeout = parsePositiveInteger(v['probe-timeout'] ?? '5000', 'Probe timeout', { max: 60000 })
  const python = v.python ?? defaultPython
  const options = { ...v, command, subcommand, endpoint, timeout, timeoutExplicit: explicitTimeout, connectTimeout: 30000, probeTimeout, python }

  if (command === 'inspect') {
    options.allPorts = v['all-ports'] ?? false
    options.interfaces = (v.interface ?? 'all').split(',')
    if (options.interfaces.some(value => !['all', 'serial', 'usb', 'usj', 'uart', 'tcp'].includes(value))) throw optionError('Unknown interface filter')
    options.fields = v.fields?.split(',')
    if (options.fields?.some(value => !fields.includes(value))) throw optionError('Unknown inspect field')
    if (options.fields && !endpoint && !v.probe && !v.select) throw optionError('--fields requires a target, --select or --probe')
    if (v.select !== undefined) {
      if (!/^\d+$/.test(v.select) || Number(v.select) < 1) throw optionError('--select must be a positive result number')
      options.select = Number(v.select)
    }
  }

  if (command === 'configure') {
    if (!endpoint) throw optionError('Specify --port or --tcp')
    if (v['network-enabled'] !== undefined && !['true', 'false'].includes(v['network-enabled'])) throw optionError('--network-enabled must be true or false')
    if ((v.ssid && v['forget-wifi']) || (v['password-stdin'] && !v.ssid) || (!v.ssid && !v['forget-wifi'] && v['network-enabled'] === undefined)) {
      throw optionError('Specify --ssid, --forget-wifi or --network-enabled')
    }
  }

  if (command === 'control') {
    if (!endpoint) throw optionError('Specify --port or --tcp')
    if (!['stop', 'reset', 'bootloader'].includes(v.action)) throw optionError('--action must be stop, reset or bootloader')
    if (v.action === 'reset' && !['soft', 'hard'].includes(v.mode)) throw optionError('--action reset requires --mode soft or hard')
    if (v.action !== 'reset' && v.mode !== undefined) throw optionError('--mode applies only to --action reset; omit it with stop or bootloader')
  }

  if (command === 'firmware') {
    if (v.mode !== undefined && !['update', 'install'].includes(v.mode)) throw optionError('Firmware mode must be update or install')
    if (v.mode === 'install' && !v.board) throw optionError('Firmware installation requires explicit --board')
    if (!endpoint && !v.board) throw optionError('Firmware requires a connected device or --board')
    if (subcommand === 'update' && (!v.port || !v.version)) throw optionError('Firmware update requires local --port and explicit --version')
    if (subcommand === 'check' && v.image) throw optionError('--image applies only to firmware update')
  }

  if (command === 'python') {
    if (!endpoint) throw optionError('Specify --port or --tcp')
    const forms = ['source', 'file', 'path'].filter(key => v[key] !== undefined)
    if (forms.length !== 1) throw optionError('Specify exactly one of --source, --file or --path')
    if (v.path !== undefined) devicePath(v.path)
    options.completion = v.completion ?? 'wait'
    if (!['wait', 'start'].includes(options.completion)) throw optionError('--completion must be wait or start')
    if (options.completion === 'start' && v.path === undefined) throw optionError('--completion start requires device --path')
    if (options.completion === 'start' && explicitTimeout) throw optionError('--timeout applies only with --completion wait')
    options.maxOutputChars = parsePositiveInteger(v['max-output-chars'] ?? String(DEFAULT_PYTHON_OUTPUT), '--max-output-chars', { max: MAX_PYTHON_OUTPUT })
  }

  if (command === 'files' || command === 'capture' || command === 'batch' || command === 'watch') {
    if (!endpoint) throw optionError('Specify --port or --tcp')
  }

  if (command === 'watch') {
    if (v.duration === undefined) throw optionError('watch requires --duration MS')
    options.duration = parsePositiveInteger(v.duration, 'Watch duration', { max: 3600000 })
  }

  if (command === 'files') {
    const pathPositional = p[2]
    if (subcommand === 'list') {
      if (pathPositional !== undefined) throw optionError('files list accepts --path DEVICE_DIR')
      options.path = v.path ?? '/'
    } else {
      if (!pathPositional) throw optionError(`files ${subcommand} requires DEVICE_PATH`)
      if (v.path !== undefined) throw optionError(`files ${subcommand} uses a positional device path`)
      options.path = pathPositional
    }
    devicePath(options.path, { root: subcommand === 'list' })
    if (subcommand === 'read') {
      if (!v.output) throw optionError('files read requires --output LOCAL_FILE')
      options.maxBytes = parsePositiveInteger(v['max-bytes'] ?? String(MAX_FILE_OUTPUT), '--max-bytes', { max: MAX_FILE_OUTPUT })
    }
    if (subcommand === 'write' && !v.file) throw optionError('files write requires --file LOCAL_FILE')
    if (subcommand === 'remove' && v.type !== undefined && !['file', 'directory'].includes(v.type)) throw optionError('--type must be file or directory')
  }

  if (command === 'capture') {
    options.source = p[1]
    if (!['camera', 'screen'].includes(options.source)) throw optionError('Capture source must be camera or screen')
    if (!v.output) throw optionError('capture requires --output LOCAL.jpg')
    options.maxBytes = parsePositiveInteger(v['max-bytes'] ?? String(MAX_FILE_OUTPUT), '--max-bytes', { max: MAX_FILE_OUTPUT })
    if (v.quality !== undefined) options.quality = parsePositiveInteger(v.quality, '--quality', { max: 100 })
    if (options.quality !== undefined && options.quality < 1) throw optionError('--quality must be 1..100')
  }

  if (command === 'batch') {
    if (!p[1]) throw optionError('batch requires PLAN.json')
    options.planFile = resolve(p[1])
  }

  if (command === 'inspect' && endpoint && v.select !== undefined) throw optionError('Use either an explicit device or --select')
  if (command === 'inspect' && v.role && !v.port) throw optionError('--role requires --port')
  return options
}

async function passwordFromInput(options) {
  if (options['password-stdin']) {
    let value = ''
    for await (const chunk of process.stdin) {
      value += chunk
      if (value.length > 4096) throw optionError('Password input is too long')
    }
    return value.replace(/\r?\n$/, '')
  }
  if (!process.stdin.isTTY) throw optionError('Use --password-stdin for non-interactive Wi-Fi configuration')
  process.stderr.write('Wi-Fi password (hidden; empty for open network): ')
  const output = new Writable({ write(_chunk, _encoding, callback) { callback() } })
  const terminal = createInterface({ input: process.stdin, output, terminal: true })
  terminal.once('SIGINT', () => process.kill(process.pid, 'SIGINT'))
  try { return await terminal.question('', { signal: options.signal }) }
  finally { terminal.close(); process.stderr.write('\n') }
}

function errorInfo(failure) {
  if (!failure) return undefined
  return { code: failure.code ?? 'COMMAND_FAILED', message: failure.message ?? 'Command failed' }
}

function attachCleanupFailure(primary, cleanupFailure) {
  if (primary && cleanupFailure) {
    primary.cleanupError = errorInfo(cleanupFailure)
    return primary
  }
  return primary ?? cleanupFailure
}

function validatePlan(plan, planFile) {
  const allowedOps = new Set(['python', 'files.list', 'files.read', 'files.write', 'files.remove', 'files.mkdir', 'capture'])
  const planError = message => error('INVALID_PLAN', message)
  if (!plan || typeof plan !== 'object' || Array.isArray(plan)
      || plan.schemaVersion !== schemaVersion || !Array.isArray(plan.operations)
      || plan.operations.length < 1 || plan.operations.length > MAX_BATCH_OPERATIONS
      || Object.keys(plan).some(key => !['schemaVersion', 'operations'].includes(key))) {
    throw planError('Batch plan must be an object with schemaVersion 1 and 1..100 operations')
  }
  const base = dirname(planFile)
  const local = value => {
    if (typeof value !== 'string' || value.length === 0 || value.includes('\0')) throw planError('Batch local paths must be nonempty strings')
    return resolve(base, value)
  }
  const planDevicePath = (value, options = {}) => devicePath(value, { ...options, fail: planError })
  return plan.operations.map((operation, index) => {
    const fail = message => { throw planError(`Batch operation ${index + 1}: ${message}`) }
    if (!operation || typeof operation !== 'object' || Array.isArray(operation)
        || typeof operation.op !== 'string' || !allowedOps.has(operation.op)) fail('unknown or malformed operation')
    const allowed = {
      python: ['op', 'source', 'file', 'path'],
      'files.list': ['op', 'path', 'recursive'],
      'files.read': ['op', 'path', 'output', 'maxBytes'],
      'files.write': ['op', 'path', 'file', 'overwrite'],
      'files.remove': ['op', 'path', 'type'],
      'files.mkdir': ['op', 'path', 'recursive'],
      capture: ['op', 'source', 'output', 'quality', 'maxBytes'],
    }[operation.op]
    if (Object.keys(operation).some(key => !allowed.includes(key))) fail('contains an unknown field')
    const copy = { ...operation }
    if (operation.op === 'python') {
      const forms = ['source', 'file', 'path'].filter(key => Object.hasOwn(operation, key))
      if (forms.length !== 1) fail('python requires exactly one of source, file or path')
      if (operation.source !== undefined && typeof operation.source !== 'string') fail('source must be a string')
      if (operation.path !== undefined) copy.path = planDevicePath(operation.path)
      if (operation.file !== undefined) copy.file = local(operation.file)
    } else if (operation.op === 'files.list') {
      copy.path = planDevicePath(operation.path ?? '/', { root: true })
      if (operation.recursive !== undefined && typeof operation.recursive !== 'boolean') fail('recursive must be boolean')
    } else if (operation.op === 'files.read') {
      copy.path = planDevicePath(operation.path)
      if (operation.output === undefined) fail('files.read requires output')
      copy.output = local(operation.output)
      copy.maxBytes = operation.maxBytes ?? MAX_FILE_OUTPUT
      if (!Number.isSafeInteger(copy.maxBytes) || copy.maxBytes < 1 || copy.maxBytes > MAX_FILE_OUTPUT) fail('maxBytes must be 1..16 MiB')
    } else if (operation.op === 'files.write') {
      copy.path = planDevicePath(operation.path)
      if (operation.file === undefined) fail('files.write requires file')
      copy.file = local(operation.file)
      if (operation.overwrite !== undefined && typeof operation.overwrite !== 'boolean') fail('overwrite must be boolean')
    } else if (operation.op === 'files.remove') {
      copy.path = planDevicePath(operation.path)
      if (operation.type !== undefined && !['file', 'directory'].includes(operation.type)) fail('type must be file or directory')
    } else if (operation.op === 'files.mkdir') {
      copy.path = planDevicePath(operation.path)
      if (operation.recursive !== undefined && typeof operation.recursive !== 'boolean') fail('recursive must be boolean')
    } else if (operation.op === 'capture') {
      if (!['camera', 'screen'].includes(operation.source)) fail('capture source must be camera or screen')
      if (operation.output === undefined) fail('capture requires output')
      copy.output = local(operation.output)
      copy.maxBytes = operation.maxBytes ?? MAX_FILE_OUTPUT
      if (!Number.isSafeInteger(copy.maxBytes) || copy.maxBytes < 1 || copy.maxBytes > MAX_FILE_OUTPUT) fail('maxBytes must be 1..16 MiB')
      if (operation.quality !== undefined && (!Number.isSafeInteger(operation.quality) || operation.quality < 1 || operation.quality > 100)) fail('quality must be 1..100')
    }
    return copy
  })
}

async function loadPlan(options, dependencies) {
  const read = dependencies.readFile ?? readFile
  let plan
  try { plan = JSON.parse(await read(options.planFile, 'utf8')) }
  catch (cause) {
    if (cause.code === 'ARGUMENT') throw cause
    throw error('INVALID_PLAN', `Cannot read batch plan: ${cause.message}`, { cause })
  }
  return validatePlan(plan, options.planFile)
}

function emit(dependencies, event) {
  dependencies.onEvent?.({ schemaVersion, ...event })
}

async function writeLocalOutput(path, data, maxBytes, dependencies) {
  if (!(data instanceof Uint8Array)) throw error('INVALID_DEVICE_RESULT', 'Device returned no binary data')
  if (data.byteLength > maxBytes) throw error('OUTPUT_TOO_LARGE', `Output exceeds ${maxBytes} bytes`)
  await (dependencies.writeFile ?? writeFile)(path, data)
  return { output: path, bytes: data.byteLength }
}

function createPythonObserver(device, request, dependencies) {
  const maxOutputChars = request.maxOutputChars ?? DEFAULT_PYTHON_OUTPUT
  const unsubscribers = []
  let outputChars = 0
  let liveOutputSeen = false
  let truncationReported = false
  let closed = false

  const reportTruncated = () => {
    if (truncationReported) return
    truncationReported = true
    emit(dependencies, { event: 'python.output', data: '', truncated: true, maxOutputChars })
  }
  const output = (text, source = 'live') => {
    if (source === 'live') liveOutputSeen = true
    if (typeof text !== 'string' || text.length === 0) return
    const remaining = Math.max(0, maxOutputChars - outputChars)
    let bounded = text.slice(0, remaining)
    // Avoid emitting a dangling high surrogate when a limit splits a pair.
    if (bounded.length && /[\uD800-\uDBFF]/.test(bounded.at(-1))) bounded = bounded.slice(0, -1)
    if (bounded) {
      outputChars += bounded.length
      emit(dependencies, { event: 'python.output', data: bounded, truncated: false })
    }
    if (bounded.length < text.length) reportTruncated()
  }
  const close = () => {
    if (closed) return
    closed = true
    for (const unsubscribe of unsubscribers.splice(0)) {
      try { unsubscribe?.() } catch { /* Cleanup must not replace the device operation result. */ }
    }
  }

  if (typeof device.on === 'function') {
    try {
      unsubscribers.push(device.on('python.output', text => output(text)))
      unsubscribers.push(device.on('log', text => emit(dependencies, { event: 'python.log', data: text })))
      unsubscribers.push(device.on('state', state => emit(dependencies, { event: 'python.state', data: state })))
    } catch (failure) {
      close()
      throw failure
    }
  }
  return {
    close,
    output(text) { output(text, 'fallback') },
    reportTruncated,
    get liveOutputSeen() { return liveOutputSeen },
    get outputChars() { return outputChars },
  }
}

async function runPython(device, request, dependencies) {
  const startedAt = Date.now()
  const observer = createPythonObserver(device, request, dependencies)
  let result
  try {
    result = await device.python.execute({
      ...request,
      ...(dependencies.signal ? { signal: dependencies.signal } : {}),
      maxOutputChars: request.maxOutputChars ?? DEFAULT_PYTHON_OUTPUT,
    })
  } finally {
    observer.close()
  }
  const durationMs = Date.now() - startedAt
  if (!observer.liveOutputSeen && result.output) observer.output(result.output)
  if (result.outputTruncated) observer.reportTruncated()
  if (request.completion === 'start') {
    if (result.done && typeof result.done.then === 'function') {
      // Start is an acknowledgement, not an owned background-job subscription.
      // Consume SDK completion rejection when disconnect releases the transport,
      // without reporting a later outcome the CLI cannot reliably observe.
      Promise.resolve(result.done).catch(() => {})
    }
    return { ok: true, started: true, path: result.path ?? request.path, durationMs }
  }
  const { output, ...metadata } = result
  const summary = { ...metadata, outputChars: observer.outputChars, durationMs }
  if (result.ok === false) throw error('PYTHON_FAILED', 'Python execution failed', { result: summary })
  return summary
}

async function runOperation(device, operation, options, dependencies) {
  const signal = dependencies.signal
  if (operation.op === 'python') {
    const request = {
      ...(operation.source !== undefined ? { source: operation.source } : {}),
      ...(operation.path !== undefined ? { path: operation.path } : {}),
      ...(operation.file !== undefined ? { source: await (dependencies.readFile ?? readFile)(operation.file, 'utf8') } : {}),
      completion: 'wait', maxOutputChars: DEFAULT_PYTHON_OUTPUT,
      ...(options.timeoutExplicit ? { timeout: options.timeout } : {}),
    }
    return runPython(device, request, dependencies)
  }
  if (operation.op === 'files.list') return device.files.list({ path: operation.path ?? '/', recursive: operation.recursive ?? false, signal })
  if (operation.op === 'files.read') {
    const data = await device.files.read({ path: operation.path, maxBytes: operation.maxBytes ?? MAX_FILE_OUTPUT, ...(options.timeoutExplicit ? { timeout: options.timeout } : {}), signal })
    return { path: operation.path, ...(await writeLocalOutput(operation.output, data, operation.maxBytes ?? MAX_FILE_OUTPUT, dependencies)) }
  }
  if (operation.op === 'files.write') {
    const data = await (dependencies.readFile ?? readFile)(operation.file)
    return device.files.write({ path: operation.path, data, overwrite: operation.overwrite ?? true, signal })
  }
  if (operation.op === 'files.remove') return device.files.remove({ path: operation.path, type: operation.type ?? 'file', signal })
  if (operation.op === 'files.mkdir') return device.files.mkdir({ path: operation.path, recursive: operation.recursive ?? false, signal })
  if (operation.op === 'capture') {
    const capture = await device.capture({ source: operation.source, format: 'jpeg', ...(operation.quality ? { quality: operation.quality } : {}), ...(options.timeoutExplicit ? { timeout: options.timeout } : {}), signal })
    const saved = await writeLocalOutput(operation.output, capture.data, operation.maxBytes ?? MAX_FILE_OUTPUT, dependencies)
    return { source: operation.source, mimeType: capture.mimeType ?? 'image/jpeg', metadata: capture.metadata ?? null, ...saved }
  }
  throw error('INVALID_PLAN', 'Unknown batch operation')
}

async function runBatch(device, options, plan, dependencies) {
  const completed = []
  for (let index = 0; index < plan.length; index++) {
    const operation = plan[index]
    emit(dependencies, { event: 'batch.operation', state: 'started', index: index + 1, op: operation.op })
    try {
      const result = await runOperation(device, operation, options, dependencies)
      if (result?.ok === false) throw error('PYTHON_FAILED', 'Python execution failed', { result })
      completed.push({ index: index + 1, operation: operation.op, result })
      emit(dependencies, { event: 'batch.operation', state: 'completed', index: index + 1, op: operation.op })
    } catch (failure) {
      const report = {
        schemaVersion, ok: false, command: 'batch', completed,
        failed: { index: index + 1, operation: operation.op, error: errorInfo(failure) },
      }
      throw error(failure.code ?? 'BATCH_FAILED', failure.message ?? 'Batch operation failed', { batchReport: report, cause: failure })
    }
  }
  return { schemaVersion, ok: true, command: 'batch', completed }
}

function shouldProbeRepl(options) {
  if (['inspect', 'configure', 'capture', 'watch'].includes(options.command)) return false
  if (options.command === 'files' && options.subcommand === 'write') return false
  if (options.command === 'firmware' && options.subcommand === 'check') return false
  return true
}

function createWatchObserver(device, options, dependencies) {
  if (typeof device.on !== 'function') throw error('EVENTS_UNAVAILABLE', 'Device Core does not expose event subscriptions')
  let eventCount = 0
  let ready = false
  let finished = false
  let lost
  let timer
  let rejectWait
  let abortHandler
  const unsubscribers = []
  const report = (name, data) => {
    if (finished) return
    eventCount++
    emit(dependencies, { event: `watch.${name}`, data })
  }
  const subscribe = (name, handler) => unsubscribers.push(device.on(name, handler))
  subscribe('python.output', data => report('output', data))
  subscribe('log', data => report('log', data))
  subscribe('state', state => {
    report('state', state)
    if (state?.state === 'ready') ready = true
    else if (state?.state === 'disconnected' && ready && !finished) {
      lost = error('SESSION_LOST', 'Device connection was lost during watch')
      rejectWait?.(lost)
    }
  })
  subscribe('diagnostic', data => report('diagnostic', data))

  function close() {
    if (finished) return
    finished = true
    clearTimeout(timer)
    if (abortHandler) dependencies.signal?.removeEventListener('abort', abortHandler)
    for (const unsubscribe of unsubscribers.splice(0)) unsubscribe?.()
  }

  return {
    close,
    async wait() {
      if (lost) throw lost
      const startedAt = Date.now()
      let cancelled = false
      try {
        await new Promise((resolve, reject) => {
          rejectWait = reject
          timer = setTimeout(resolve, options.duration)
          abortHandler = () => { cancelled = true; resolve() }
          dependencies.signal?.addEventListener('abort', abortHandler, { once: true })
          if (dependencies.signal?.aborted) abortHandler()
          if (lost) reject(lost)
        })
      } finally {
        close()
      }
      return { durationMs: Date.now() - startedAt, eventCount, ...(cancelled ? { cancelled: true } : {}) }
    },
  }
}

export async function runCli(options, dependencies = {}) {
  if (options.help) return { help: usage }
  const startedAt = Date.now()
  let endpoint = options.endpoint
  let plan
  if (options.command === 'batch') {
    try { plan = await loadPlan(options, dependencies) }
    catch (failure) {
      emit(dependencies, { event: 'duration', milliseconds: Date.now() - startedAt })
      throw failure
    }
  }
  if (options.command === 'inspect' && !endpoint && !options.select) {
    const listing = await (dependencies.inventory ?? inventory)({ ...options, signal: dependencies.signal, timeout: Math.min(options.timeout, 10000) }, dependencies)
    if (listing.sourcesAvailable === 0) throw error('ENUMERATION_UNAVAILABLE', 'No inventory source succeeded; check Python/pyserial, multicast-dns and permissions')
    emit(dependencies, { event: 'duration', milliseconds: Date.now() - startedAt })
    return listing
  }
  if (options.command === 'inspect' && !endpoint && options.select) {
    const listing = await (dependencies.inventory ?? inventory)({ ...options, signal: dependencies.signal, timeout: Math.min(options.timeout, 10000) }, dependencies)
    endpoint = listing.devices.find(row => row.number === options.select)
    if (!endpoint) {
      emit(dependencies, { event: 'duration', milliseconds: Date.now() - startedAt })
      throw error('DEVICE_NOT_FOUND', 'Selected result number is unavailable in this enumeration')
    }
  }
  const offline = options.command === 'firmware' && options.board
  const makeDevice = dependencies.createDevice ?? createDevice
  const device = makeDevice(endpoint && !offline ? { transport: (dependencies.transportFor ?? transportFor)(endpoint, options) } : {})
  let watchObserver
  let primary
  let result
  try {
    if (options.command === 'watch') watchObserver = createWatchObserver(device, options, dependencies)
    if (endpoint && !offline) {
      emit(dependencies, { event: 'state', state: 'connecting' })
      await device.connect({
        role: endpoint.interface === 'serial' ? 'unknown' : endpoint.interface,
        timeout: options.connectTimeout,
        signal: dependencies.signal,
        probeRepl: shouldProbeRepl(options),
        ...(endpoint.advertisement ? { expectedDevice: endpoint.advertisement } : {}),
      })
      emit(dependencies, { event: 'state', state: 'connected' })
    }
    if (options.command === 'inspect') result = await device.inspect({ fields: options.fields, timeout: options.timeout, signal: dependencies.signal })
    else if (options.command === 'configure') {
      const network = {}
      if (options.ssid) network.credentials = { ssid: options.ssid, password: await (dependencies.readPassword ?? passwordFromInput)({ ...options, signal: dependencies.signal }) }
      if (options['forget-wifi']) network.credentials = null
      if (options['network-enabled'] !== undefined) network.enabled = options['network-enabled'] === 'true'
      await device.configure({ network, timeout: options.timeout, signal: dependencies.signal })
      result = { ok: true }
    } else if (options.command === 'control') {
      result = await device.control({ action: options.action, ...(options.mode ? { mode: options.mode } : {}), timeout: options.timeout, signal: dependencies.signal })
    } else if (options.command === 'firmware') {
      let catalog = options.catalog
      const mode = options.mode ?? 'update'
      if (catalog !== undefined && !/^https:\/\//.test(catalog)) catalog = JSON.parse(await (dependencies.readFile ?? readFile)(catalog, 'utf8'))
      const checked = await device.firmware.check({ catalog, mode, ...(endpoint && !offline ? {} : { identity: { board: options.board, firmwareVersion: options['current-version'] ?? 'unknown' } }), ...(options.version ? { version: options.version } : {}), signal: dependencies.signal })
      if (options.subcommand === 'check') result = checked
      else result = await device.firmware.update({ mode, release: checked.release, board: options.board, ...(options.image ? { data: new Uint8Array(await (dependencies.readFile ?? readFile)(options.image)) } : {}), openLoader: ({ signal }) => (dependencies.openLoader ?? openNodeRom)({ port: options.port, python: options.python, signal }), onPhase: message => emit(dependencies, { event: 'firmware.phase', message }), signal: dependencies.signal })
    } else if (options.command === 'python') {
      const request = {
        ...(options.source !== undefined ? { source: options.source } : {}),
        ...(options.file !== undefined ? { source: await (dependencies.readFile ?? readFile)(options.file, 'utf8') } : {}),
        ...(options.path !== undefined ? { path: options.path } : {}),
        completion: options.completion, maxOutputChars: options.maxOutputChars,
        ...(options.timeoutExplicit ? { timeout: options.timeout } : {}),
      }
      result = await runPython(device, request, dependencies)
    } else if (options.command === 'files') {
      const signal = dependencies.signal
      if (options.subcommand === 'list') result = await device.files.list({ path: options.path, recursive: options.recursive ?? false, signal })
      else if (options.subcommand === 'read') {
        const data = await device.files.read({ path: options.path, maxBytes: options.maxBytes, ...(options.timeoutExplicit ? { timeout: options.timeout } : {}), signal })
        result = { path: options.path, ...(await writeLocalOutput(options.output, data, options.maxBytes, dependencies)) }
      } else if (options.subcommand === 'write') {
        const data = await (dependencies.readFile ?? readFile)(options.file)
        result = await device.files.write({ path: options.path, data, overwrite: !options['no-overwrite'], signal })
      } else if (options.subcommand === 'remove') result = await device.files.remove({ path: options.path, type: options.type ?? 'file', signal })
      else result = await device.files.mkdir({ path: options.path, recursive: options.recursive ?? false, signal })
    } else if (options.command === 'capture') {
      const capture = await device.capture({ source: options.source, format: 'jpeg', ...(options.quality ? { quality: options.quality } : {}), ...(options.timeoutExplicit ? { timeout: options.timeout } : {}), signal: dependencies.signal })
      const saved = await writeLocalOutput(options.output, capture.data, options.maxBytes, dependencies)
      result = { source: options.source, mimeType: capture.mimeType ?? 'image/jpeg', metadata: capture.metadata ?? null, ...saved }
    } else if (options.command === 'batch') result = await runBatch(device, options, plan, dependencies)
    else if (options.command === 'watch') result = await watchObserver.wait()
  } catch (failure) { primary = failure }
  if (!primary && options.command === 'watch' && result?.cancelled && dependencies.signal?.aborted) {
    primary = error('CANCELLED', 'Watch cancelled', { result })
  }
  watchObserver?.close()
  try { await device.disconnect() }
  catch (cleanupFailure) { primary = attachCleanupFailure(primary, cleanupFailure) }
  emit(dependencies, { event: 'state', state: 'disconnected' })
  const durationMs = Date.now() - startedAt
  emit(dependencies, { event: 'duration', milliseconds: durationMs })
  if (primary) {
    if (primary.batchReport && primary.cleanupError) primary.batchReport.cleanupError = primary.cleanupError
    if (options.command === 'batch' && !primary.batchReport) {
      const completed = result?.completed ?? []
      primary.batchReport = {
        schemaVersion, ok: false, command: 'batch', completed,
        failed: { index: completed.length + 1, operation: completed.length ? 'disconnect' : 'connect', error: errorInfo(primary) },
      }
    }
    throw primary
  }
  if (result && typeof result === 'object' && !Array.isArray(result) && result.command === 'batch') result.durationMs = durationMs
  return result
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
  if (!result?.devices) return JSON.stringify(result, null, 2)
  const headers = ['#', 'Group', 'Interface', 'Address', 'Status', 'Board', 'Firmware', 'Device ID', 'USB serial', 'Hostname', 'USB VID:PID', 'USB product', 'Error']
  const rows = result.devices.map(row => [row.number, row.group, row.interface, row.address, row.status, row.identity?.board, row.identity?.firmwareVersion, row.identity?.deviceId, row.usb?.serialNumber, row.identity?.hostname, row.usb?.vendorId == null ? null : `${row.usb.vendorId.toString(16).padStart(4, '0')}:${(row.usb.productId ?? 0).toString(16).padStart(4, '0')}`, row.usb?.product, row.error?.code].map(safe))
  if (!rows.length) return 'No devices found.'
  const widths = headers.map((header, index) => Math.max(stringWidth(header), ...rows.map(row => stringWidth(row[index]))))
  if (widths.reduce((sum, width) => sum + width, 0) + 2 * (headers.length - 1) <= columns) {
    return [headers, ...rows].map(row => row.map((value, index) => value + ' '.repeat(widths[index] - stringWidth(value))).join('  ').trimEnd()).join('\n')
  }
  const labelWidth = Math.max(...headers.map(value => stringWidth(value)))
  const indent = ' '.repeat(labelWidth + 2)
  return rows.map(row => headers.map((header, index) => {
    const lines = wrapCell(row[index], Math.max(2, columns - indent.length))
    return `${header.padEnd(labelWidth)}: ${lines.join(`\n${indent}`)}`
  }).join('\n')).join('\n\n')
}

export function successEnvelope(command, result) {
  if (result?.command === 'batch' && result.ok === false) return result
  return { schemaVersion, ok: true, command, result }
}

export function failureEnvelope(command, failure) {
  if (failure?.batchReport) return failure.batchReport
  return {
    schemaVersion, ok: false, command,
    error: errorInfo(failure),
    ...(failure?.result ? { result: failure.result } : {}),
    ...(failure?.cleanupError ? { cleanupError: failure.cleanupError } : {}),
  }
}

function formatCliResult(command, result) {
  if (command === 'python') {
    if (result?.started) return `Python start acknowledged for ${result.path}. Disconnected after ${result.durationMs} ms.`
    return `Python completed in ${result?.durationMs ?? 0} ms; observed output was sent to stderr.`
  }
  if (command === 'batch') {
    const visible = structuredClone(result)
    for (const entry of visible.completed ?? []) {
      if (entry.result && typeof entry.result === 'object') delete entry.result.output
    }
    return formatResult(visible)
  }
  return formatResult(result)
}

function mainCommandName(options) {
  return options.command === 'firmware' || options.command === 'files'
    ? [options.command, options.subcommand].filter(Boolean).join(' ') : options.command
}

// Keep command identity even if validation rejects its options. This tolerant
// parse is used only for diagnostics; parseCli still enforces strict options.
function commandContext(args) {
  try {
    const { positionals } = parseCommandOptions(args, false)
    return { command: positionals[0] ?? 'inspect', subcommand: positionals[1] }
  } catch {
    return { command: args[0] && !args[0].startsWith('-') ? args[0] : 'unknown' }
  }
}

function isMain() {
  try { return Boolean(process.argv[1]) && pathToFileURL(realpathSync(process.argv[1])).href === import.meta.url }
  catch { return false }
}

if (isMain()) {
  let signalName
  const jsonRequested = process.argv.includes('--json')
  const controller = new AbortController()
  const abort = name => {
    signalName = name
    controller.abort(error('CANCELLED', `Command cancelled by ${name}`))
  }
  const onInterrupt = () => abort('SIGINT')
  const onTerminate = () => abort('SIGTERM')
  process.once('SIGINT', onInterrupt)
  process.once('SIGTERM', onTerminate)
  let options = commandContext(process.argv.slice(2))
  try {
    options = parseCli(process.argv.slice(2))
    const command = options.help ? 'help' : mainCommandName(options)
    const result = await runCli(options, {
      signal: controller.signal,
      onEvent: event => process.stderr.write(`${JSON.stringify(event)}\n`),
      onPhase: message => process.stderr.write(`${JSON.stringify({ schemaVersion, event: 'firmware.phase', message })}\n`),
    })
    if (options.help) {
      const help = { usage, capabilities: cliCapabilities }
      process.stdout.write(options.json ? JSON.stringify(successEnvelope('help', help)) + '\n' : usage + '\n')
    }
    else {
      const envelope = successEnvelope(command, result)
      process.stdout.write(options.json ? JSON.stringify(envelope) + '\n' : formatCliResult(command, result) + '\n')
      for (const warning of result?.warnings ?? []) process.stderr.write(`${JSON.stringify({ schemaVersion, event: 'warning', source: warning.source, message: warning.message })}\n`)
    }
  } catch (failure) {
    const command = options.help ? 'help' : mainCommandName(options)
    const envelope = failureEnvelope(command, failure)
    if (failure.code === 'ARGUMENT' || failure.code === 'INVALID_PLAN' || failure.code?.startsWith('ERR_PARSE_ARGS')) {
      envelope.error ??= errorInfo(failure)
      process.exitCode = 2
    } else process.exitCode = signalName === 'SIGINT' ? 130 : signalName === 'SIGTERM' ? 143 : 1
    if (options.json || jsonRequested) process.stdout.write(JSON.stringify(envelope) + '\n')
    else process.stdout.write(`${envelope.error?.message ?? 'Command failed'}\n`)
    process.stderr.write(`${JSON.stringify({ schemaVersion, event: signalName ? 'cancelled' : 'error', ...errorInfo(failure), ...(failure.cleanupError ? { cleanupError: failure.cleanupError } : {}) })}\n`)
  } finally {
    process.removeListener('SIGINT', onInterrupt)
    process.removeListener('SIGTERM', onTerminate)
  }
}

export { defaultPython, filterSerialPorts, inventory, listSerial, serialEntry, transportFor }

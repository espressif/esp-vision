// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { realpath } from 'node:fs/promises'
import { setTimeout as delay } from 'node:timers/promises'
import { createDevice } from './index.js'
import { defaultPython, listSerial } from './node_inventory.js'
import { serialTransport } from './node_transport.js'

const ROM_PIDS = new Set([2, 9, 18, 32])
// Confirmed ESP-VISION runtime composites: CDC+MSC and CDC+MSC+UVC.
const RUNTIME_PIDS = new Set([0x4003, 0x4023])
const UNIQUE_ID_SOURCE = "import machine,ubinascii\nprint('__EV_ROM_ID__'+ubinascii.hexlify(machine.unique_id()).decode())"
function fail(code, message, cause) { return Object.assign(new Error(message), { code, ...(cause ? { cause } : {}) }) }
function checkAbort(signal) {
  if (!signal?.aborted) return
  throw signal.reason instanceof Error ? signal.reason : Object.assign(new Error('ROM port preparation cancelled'), { name: 'AbortError' })
}
function errorInfo(error) { return { code: error?.code ?? error?.name ?? 'ERROR', message: String(error?.message ?? error).slice(0, 512) } }
function mac(value) {
  if (typeof value !== 'string') return null
  const result = value.replace(/[:-]/g, '').toLowerCase()
  return /^[0-9a-f]{12}$/.test(result) ? result : null
}
function comKey(value) {
  return typeof value === 'string' && /^(?:\\\\\.\\)?COM[1-9]\d*$/i.test(value)
    ? value.replace(/^\\\\\.\\/, '').toUpperCase() : null
}
function physicalLocation(value) {
  // pyserial adds :configuration.interface (Windows uses :x.interface).
  return typeof value === 'string' && value.trim() ? value.trim().replace(/:(?:\d+|x)\.\d+$/i, '') : null
}
function nativeRom(port) { return port.vendorId === 0x303a && ROM_PIDS.has(port.productId) }
function nativeRuntime(port) { return port.vendorId === 0x303a && RUNTIME_PIDS.has(port.productId) }

/** Resolve only the selected port. Descriptors authorize a handoff, never a flash. */
export async function prepareNodeRomPort({ port, python = defaultPython, signal, timeout = 5000, expectedIdentity } = {}, dependencies = {}) {
  if (typeof port !== 'string' || !port || /[\x00-\x1f\x7f]/.test(port)) throw new TypeError('ROM preparation requires a local port')
  if (!Number.isSafeInteger(timeout) || timeout <= 0 || timeout > 5000) throw new RangeError('ROM preparation timeout must be positive and at most 5000 ms')
  const required = expectedIdentity == null ? null : { mac: mac(expectedIdentity.mac), location: physicalLocation(expectedIdentity.location) }
  if (required && (!required.mac || !required.location)) throw new TypeError('Bound ROM preparation requires a MAC and physical USB location')
  const enumerate = dependencies.listSerial ?? listSerial
  const resolvePath = dependencies.realpath ?? realpath
  const now = dependencies.now ?? (() => performance.now())
  const sleep = dependencies.sleep ?? ((ms, abortSignal) => delay(ms, undefined, { signal: abortSignal }))
  const makeDevice = dependencies.createDevice ?? createDevice
  const makeTransport = dependencies.serialTransport ?? serialTransport
  checkAbort(signal)
  const settleDeadline = now() + timeout
  let canonical = comKey(port)
  while (!canonical) {
    checkAbort(signal)
    try {
      canonical = await resolvePath(port)
      if (typeof canonical !== 'string' || !canonical) throw fail('ROM_PORT_LOST', 'The selected path has no canonical serial address')
    }
    catch (error) {
      checkAbort(signal)
      if (error.code !== 'ENOENT' || now() >= settleDeadline) throw fail('ROM_PORT_LOST', 'The selected serial path is unavailable; select its current port', error)
      await sleep(Math.min(50, settleDeadline - now()), signal)
    }
  }
  checkAbort(signal)
  let ports
  try { ports = await enumerate({ python, signal, timeout }) }
  catch (error) {
    checkAbort(signal)
    if (required) throw fail('ROM_PORT_IDENTITY_REQUIRED', 'Cannot confirm the bound runtime descriptors', error)
    // A known OS path may still be used without descriptors, but never probed
    // as an EV-MUX runtime or rebound to a different port.
    return { port: canonical, expectedMac: null, transition: null }
  }
  checkAbort(signal)
  if (!Array.isArray(ports)) throw fail('ROM_PORT_ENUMERATION_FAILED', 'Serial enumeration returned invalid descriptors')
  const selected = ports.filter(item => item.path === port || item.path === canonical || (comKey(item.path) && comKey(item.path) === canonical))
  if (selected.length > 1) throw fail('ROM_PORT_AMBIGUOUS', 'The selected port has ambiguous descriptors')
  if (!selected.length) {
    if (required) throw fail('ROM_PORT_LOST', 'The bound runtime port is no longer enumerated')
    return { port: canonical, expectedMac: null, transition: null }
  }
  const current = selected[0]
  const expectedMac = mac(current.serialNumber)
  const location = physicalLocation(current.location)
  if (required && (expectedMac !== required.mac || location !== required.location)) throw fail('ROM_RUNTIME_IDENTITY_MISMATCH', 'The selected runtime descriptors changed before recovery')
  if (nativeRom(current)) return {
    port: current.path, expectedMac,
    transition: { requested: false, observed: 'rom', selectedPort: port, port: current.path, serial: expectedMac, location },
  }
  if (!nativeRuntime(current)) return { port: canonical, expectedMac: null, transition: null }
  if (!expectedMac || !location) throw fail('ROM_PORT_IDENTITY_REQUIRED', 'Native USB runtime requires a MAC serial and physical USB location')

  const transition = { requested: false, selectedPort: port, runtimePort: current.path, expectedMac, location,
    command: { attempted: false, acknowledged: false, outcome: 'not_attempted' }, cleanup: null, observed: null }
  let device, primary
  try {
    device = makeDevice({ transport: makeTransport({ serial: current.path, role: 'usb', python }) })
    const connected = await device.connect({ role: 'usb', probeRepl: false, autoBaud: false, timeout, signal })
    checkAbort(signal)
    const owners = connected?.capabilities?.sinks?.filter(sink => sink.owner === true) ?? []
    if (owners.length !== 1 || owners[0].id !== 'usb' || owners[0].kind !== 'usb_otg_cdc' || owners[0].state !== 'active' || owners[0].ready !== true) {
      throw fail('ROM_RUNTIME_INTERFACE_MISMATCH', 'The selected runtime did not establish the active USB owner')
    }
    let runtimeMac = connected?.identity?.deviceId
    let identitySource = 'runtime.deviceId'
    if (runtimeMac == null) {
      const result = await device.python.execute({ source: UNIQUE_ID_SOURCE, completion: 'wait', timeout, maxOutputChars: 512, signal })
      checkAbort(signal)
      const matches = typeof result?.output === 'string' ? [...result.output.matchAll(/^__EV_ROM_ID__([0-9a-f]{12})\r?$/gim)] : []
      if (result?.ok !== true || result.outputTruncated !== false || matches.length !== 1 || /Traceback/.test(result.output)) {
        throw fail('ROM_RUNTIME_IDENTITY_UNAVAILABLE', 'The legacy runtime did not return one complete unique_id')
      }
      runtimeMac = matches[0][1]
      identitySource = 'machine.unique_id'
    }
    if (mac(runtimeMac) !== expectedMac) throw fail('ROM_RUNTIME_IDENTITY_MISMATCH', 'Runtime identity does not match the selected USB serial')
    transition.identitySource = identitySource
    checkAbort(signal)
    transition.requested = true
    transition.command = { attempted: true, acknowledged: false, outcome: 'unknown' }
    try {
      const result = await device.control({ action: 'bootloader', timeout, signal })
      checkAbort(signal)
      if (result?.ok !== true || result.operation !== 'bootloader') throw fail('ROM_BOOTLOADER_REJECTED', 'Runtime did not accept the bootloader command')
      transition.command = { attempted: true, acknowledged: result.acknowledged === true, outcome: result.outcome ?? 'unknown' }
    } catch (error) {
      checkAbort(signal)
      if (error.reset?.outcome !== 'unknown' && error.outcome !== 'unknown') throw error
      transition.command = { attempted: true, acknowledged: false, outcome: 'unknown', error: errorInfo(error) }
    }
  } catch (error) { primary = error }
  finally {
    if (device) {
      try { await device.disconnect(); transition.cleanup = { ok: true } }
      catch (error) { transition.cleanup = { ok: false, error: errorInfo(error) } }
    }
  }
  try { checkAbort(signal) } catch (error) { primary ??= error }
  if (primary) {
    primary.transition = transition
    if (transition.cleanup?.ok === false) primary.cleanupError = transition.cleanup.error
    throw primary
  }

  // No reset replay: only the selected physical device's ROM may finish the
  // handoff. A successful match does not turn an unknown command into an ACK.
  const deadline = now() + timeout
  while (now() < deadline) {
    checkAbort(signal)
    const remaining = Math.max(1, Math.ceil(deadline - now()))
    let candidates
    try { candidates = await enumerate({ python, signal, timeout: remaining }) }
    catch (error) {
      checkAbort(signal)
      throw Object.assign(fail('ROM_PORT_REBIND_FAILED', 'Cannot enumerate the selected device after bootloader submission', error), { transition })
    }
    checkAbort(signal)
    if (now() >= deadline) break
    if (!Array.isArray(candidates)) throw Object.assign(fail('ROM_PORT_ENUMERATION_FAILED', 'Serial enumeration returned invalid descriptors'), { transition })
    const matches = candidates.filter(item => nativeRom(item) && mac(item.serialNumber) === expectedMac && physicalLocation(item.location) === location)
    if (matches.length > 1) throw Object.assign(fail('ROM_PORT_AMBIGUOUS', 'Multiple ROM ports match the selected device'), { transition })
    if (matches.length === 1) {
      const selectedRom = matches[0]
      transition.observed = 'rom'
      transition.port = selectedRom.path
      return { port: selectedRom.path, expectedMac, transition }
    }
    await sleep(Math.min(50, Math.max(1, deadline - now())), signal)
  }
  throw Object.assign(fail('ROM_PORT_REBIND_TIMEOUT', 'The selected device did not expose one matching ROM port within the deadline'), { transition })
}


/** Only for the caller's first read-only initialize failure, never read/write. */
export async function recoverNodeRomPort(previous, { python = defaultPython, signal, timeout = 5000 } = {}, dependencies = {}) {
  checkAbort(signal)
  if (!Number.isSafeInteger(timeout) || timeout <= 0 || timeout > 5000) throw new RangeError('ROM recovery timeout must be positive and at most 5000 ms')
  const expectedMac = mac(previous?.expectedMac)
  const location = physicalLocation(previous?.transition?.location)
  if (!expectedMac || !location || previous?.transition?.observed !== 'rom' || previous.transition.requested !== false) return null
  if (mac(previous.transition.serial) !== expectedMac) return null
  const enumerate = dependencies.listSerial ?? listSerial
  const now = dependencies.now ?? (() => performance.now())
  const deadline = now() + timeout
  let ports
  try { ports = await enumerate({ python, signal, timeout }) }
  catch (error) {
    checkAbort(signal)
    throw fail('ROM_PORT_ENUMERATION_FAILED', 'Cannot verify the selected device for read-only initialization recovery', error)
  }
  checkAbort(signal)
  if (now() >= deadline) throw fail('ROM_PORT_REBIND_TIMEOUT', 'Read-only initialization recovery enumeration exceeded its deadline')
  if (!Array.isArray(ports)) throw fail('ROM_PORT_ENUMERATION_FAILED', 'Serial enumeration returned invalid descriptors')
  const sameDevice = item => mac(item.serialNumber) === expectedMac && physicalLocation(item.location) === location
  const existingRom = ports.filter(item => nativeRom(item) && sameDevice(item))
  if (existingRom.length > 1) throw fail('ROM_PORT_AMBIGUOUS', 'Multiple ROM ports match the selected device')
  if (existingRom.length) return null
  const runtimes = ports.filter(nativeRuntime)
  const conflicting = runtimes.filter(item => (mac(item.serialNumber) === expectedMac || physicalLocation(item.location) === location) && !sameDevice(item))
  if (conflicting.length) throw fail('ROM_RUNTIME_IDENTITY_MISMATCH', 'Runtime descriptors do not match the previously selected ROM device')
  const matches = runtimes.filter(sameDevice)
  if (matches.length > 1) throw fail('ROM_PORT_AMBIGUOUS', 'Multiple runtime ports match the selected device')
  if (!matches.length) return null
  // Revalidate at prepare's fresh enumeration, before opening EV-MUX. An OS port
  // may have been reused for a different board since the recovery snapshot.
  const result = await prepareNodeRomPort({ port: matches[0].path, python, signal, timeout,
    expectedIdentity: { mac: expectedMac, location } }, dependencies)
  if (result.expectedMac !== expectedMac || result.transition?.observed !== 'rom' || physicalLocation(result.transition.location) !== location) {
    throw fail('ROM_RUNTIME_IDENTITY_MISMATCH', 'Recovery did not preserve the selected physical device identity')
  }
  result.transition.recovery = { previousPort: previous.port, runtimePid: matches[0].productId }
  return result
}

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { setTimeout as delay } from 'node:timers/promises'
import { clearDownloadLatch } from './rom_reset.js'

const ROM_PIDS = new Set([2, 9, 18, 32])
const fail = (code, message) => Object.assign(new Error(message), { code })
const normalizeMac = value => typeof value === 'string' && /^[0-9a-f]{12}$/i.test(value.replace(/[:-]/g, '')) ? value.replace(/[:-]/g, '').toLowerCase() : null
const location = value => typeof value === 'string' && value.trim() ? value.trim().replace(/:(?:\d+|x)\.\d+$/i, '') : null
const isRom = port => port.vendorId === 0x303a && ROM_PIDS.has(port.productId)
const errorInfo = error => ({ code: error.code ?? 'ROM_PROBE_FAILED', message: error.message })

/** Active detection only: one identity-bound ROM -> normal SPI boot attempt. */
export async function probeNodeRom(selected, { timeout = 5000, signal } = {}, dependencies = {}) {
  const mac = normalizeMac(selected.serialNumber), usbLocation = location(selected.location)
  if (!isRom(selected) || !mac || !usbLocation) throw fail('ROM_PORT_IDENTITY_REQUIRED', 'ROM detection requires a MAC and physical USB location')
  if (!Number.isSafeInteger(timeout) || timeout <= 0) throw new RangeError('ROM probe timeout must be positive')
  signal?.throwIfAborted()
  const enumerate = dependencies.listSerial ?? (await import('./node_inventory.js')).listSerial
  const open = dependencies.openNodeRom ?? (await import('./node_rom.js')).openNodeRom
  const now = dependencies.now ?? (() => performance.now())
  const sleep = dependencies.sleep ?? ((ms, abortSignal) => delay(ms, undefined, { signal: abortSignal }))
  const deadline = now() + timeout
  const operation = new AbortController()
  const activeSignal = signal ? AbortSignal.any([signal, operation.signal]) : operation.signal
  const timer = setTimeout(() => operation.abort(fail('ROM_PROBE_TIMEOUT', 'Normal-boot detection timed out')), timeout)
  const same = port => port.vendorId === 0x303a && normalizeMac(port.serialNumber) === mac && location(port.location) === usbLocation
  const result = { attempted: false, outcome: 'not-attempted', mac, location: usbLocation, selectedPort: selected.path }
  let loader, lastRom = null
  try {
    loader = open({ port: selected.path, signal: activeSignal }, {
      // Never turn a stale probe selection into a bootloader request against a
      // runtime. ROM initialization recovery must not replay this SPI attempt.
      async preparePort() {
        const ports = await enumerate({ signal: activeSignal, timeout: Math.max(1, Math.ceil(deadline - now())) })
        const current = ports.filter(same)
        if (current.length === 1 && !isRom(current[0])) throw Object.assign(fail('ROM_ALREADY_RUNTIME', 'Selected device already returned to runtime'), { runtime: current[0] })
        const matches = ports.filter(port => port.path === selected.path && isRom(port) && same(port))
        if (matches.length !== 1) throw fail('ROM_PORT_IDENTITY_CHANGED', 'Selected ROM descriptors changed before detection')
        return { port: selected.path, expectedMac: mac, transition: { requested: false, observed: 'rom', serial: mac, location: usbLocation } }
      },
      recoverPort: async () => null,
    })
    await loader.initialize()
    activeSignal.throwIfAborted()
    if (loader.chipMac !== mac) throw fail('ROM_IDENTITY_CHANGED', 'ROM MAC differs from the selected device')
    if (![0x3280, 0x3253, 0x3231].includes(loader.chipFamily)) throw fail('ROM_CHIP_UNSUPPORTED', 'Unsupported chip for normal-boot detection')
    const security = await loader.getSecurityInfo()
    if (!Number.isSafeInteger(security.flags) || security.flags < 0 || security.flags > 0xffffffff
        || !Number.isSafeInteger(security.flashCryptCnt) || security.flashCryptCnt < 0 || security.flashCryptCnt > 0xffffffff
        || (security.flags & 5) || security.flashCryptCnt.toString(2).replace(/0/g, '').length % 2) {
      throw fail('ROM_SECURITY_UNSUPPORTED', 'Cannot qualify a normal-boot probe in this security mode')
    }
    await clearDownloadLatch(loader.chipFamily, loader)
    activeSignal.throwIfAborted()
    result.attempted = true
    result.outcome = 'unknown'
    try { await loader.hardReset(); result.submitted = true }
    catch (error) { result.resetError = errorInfo(error) }
    try { await loader.disconnect(); result.cleanup = { ok: true } }
    catch (error) { result.cleanup = { ok: false, error: errorInfo(error) } }
    loader = null
    while (now() < deadline) {
      activeSignal.throwIfAborted()
      const ports = await enumerate({ signal: activeSignal, timeout: Math.max(1, Math.ceil(deadline - now())) })
      activeSignal.throwIfAborted()
      if (now() >= deadline) break
      const matches = ports.filter(same)
      if (matches.length > 1) throw fail('ROM_PORT_AMBIGUOUS', 'Multiple endpoints match the selected device after normal reset')
      const port = matches[0]
      lastRom = port && isRom(port) ? port : null
      if (port && !isRom(port)) return { port, probe: { ...result, outcome: 'runtime' } }
      await sleep(Math.min(50, Math.max(1, deadline - now())), activeSignal)
    }
    return { port: lastRom, probe: { ...result, outcome: lastRom ? 'rom' : 'no-runtime' } }
  } catch (error) {
    signal?.throwIfAborted()
    if (!result.attempted && error.runtime && same(error.runtime) && !isRom(error.runtime)) {
      return { port: error.runtime, probe: { ...result, outcome: 'runtime' } }
    }
    if (result.attempted && (activeSignal.aborted || error.code === 'ROM_PROBE_TIMEOUT')) {
      return { port: lastRom, probe: { ...result, outcome: lastRom ? 'rom' : 'no-runtime', error: errorInfo(error) } }
    }
    error.romProbe = result
    throw error
  } finally {
    clearTimeout(timer)
    await loader?.disconnect()
  }
}

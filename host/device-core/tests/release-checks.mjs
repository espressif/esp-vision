// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { realpath } from 'node:fs/promises'
import { inventory } from '../src/node.js'

export const RELEASE_CHECKS = Object.freeze(['discovery', 'communication', 'reconnect'])
const fail = (code, message) => Object.assign(new Error(message), { code })
const canonical = async path => { try { return await realpath(path) } catch { return path } }

/** Real target operations through the public SDK; no separate protocol client. */
export async function runReleaseCheck(name, device, options, { signal, nonce, discover = inventory } = {}) {
  const inspect = () => device.inspect({ fields: ['identity', 'runtime'], signal })
  const before = await inspect()
  const identity = before.identity
  if (!identity?.deviceId || !identity.board || !identity.firmwareVersion) throw fail('IDENTITY_INVALID', 'Release checks require a complete runtime identity')
  const sameIdentity = info => {
    if (['deviceId', 'board', 'firmwareVersion'].some(key => info.identity?.[key] !== identity[key])) throw fail('IDENTITY_CHANGED', 'Runtime identity changed during acceptance')
    if (info.runtime?.vmReady !== true) throw fail('VM_NOT_READY', 'Runtime VM is not ready')
  }
  const execute = async (source, expected) => {
    const result = await device.python.execute({ source, signal, timeout: 15000, maxOutputChars: 16384 })
    if (!result.ok || result.outputTruncated || result.output.replaceAll('\r', '').trim() !== expected) throw fail('ROUNDTRIP_MISMATCH', 'Python output did not match the current request')
  }
  if (name === 'discovery') {
    const marker = `EV_DISCOVERY_${nonce}`
    const variable = `_evdiscovery_${nonce}`
    try {
      await execute(`${variable}=${JSON.stringify(marker)};print(${variable})`, marker)
      const found = await discover({ interfaces: [options.tcp ? 'tcp' : 'serial'], probe: false, timeout: 3000, signal })
      if (!found.sourcesAvailable || found.warnings.length) throw fail('DISCOVERY_UNAVAILABLE', 'Passive inventory did not complete without warnings')
      let matches
      if (options.tcp) {
        matches = found.devices.filter(row => row.identity?.deviceId === identity.deviceId && row.port === options.port &&
          (row.host === options.host || row.advertisement?.addresses?.includes(options.host) || row.identity?.hostname === options.host))
      } else {
        const selected = await canonical(options.serial)
        matches = []
        for (const row of found.devices) if (await canonical(row.address) === selected) matches.push(row)
      }
      if (matches.length !== 1) throw fail('DISCOVERY_TARGET_MISSING', 'Passive inventory must contain exactly the selected endpoint')
      await execute(`print(${variable})`, marker)
      sameIdentity(await inspect())
      return { interface: options.role, status: matches[0].status, deviceId: identity.deviceId, vmStatePreserved: true, probe: false }
    } finally {
      // Delete only our nonce-scoped variable, including after enumeration failure.
      const result = await device.python.execute({ source: `globals().pop(${JSON.stringify(variable)},None);print('EV_DISCOVERY_CLEAN')`, signal: AbortSignal.timeout(5000), timeout: 5000, maxOutputChars: 256 })
      if (!result.ok || result.outputTruncated || result.output.trim() !== 'EV_DISCOVERY_CLEAN') throw fail('DISCOVERY_CLEANUP_FAILED', 'Could not release discovery sentinel')
    }
  }
  if (name === 'communication') {
    const rounds = 5
    for (let round = 0; round < rounds; round++) {
      // Short source stays in RAM; the 4 KiB UTF-8 output crosses frame boundaries.
      const prefix = `EV_COMM_${nonce}_${round}:`
      const unit = 'Vision-视觉-123|'
      const expected = prefix + unit.repeat(256)
      await execute(`print(${JSON.stringify(prefix)}+${JSON.stringify(unit)}*256)`, expected)
      sameIdentity(await inspect())
    }
    return { rounds, unicode: true, rpcBetweenExecutions: true, deviceId: identity.deviceId }
  }
  if (name === 'reconnect') {
    for (let round = 0; round < 3; round++) {
      await device.disconnect()
      await device.connect({ role: options.role, timeout: 15000, signal })
      sameIdentity(await inspect())
      const marker = `EV_RECONNECT_${nonce}_${round}`
      await execute(`print(${JSON.stringify(marker)})`, marker)
    }
    return { cycles: 3, deviceId: identity.deviceId, freshMarkers: true }
  }
  throw fail('UNKNOWN_CHECK', 'Unknown release check')
}

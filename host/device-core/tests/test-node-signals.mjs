// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { setSerialSignals } from '../src/node_signals.js'

const unsupported = () => new Error('Operation not supported, cannot set')
test('Linux USJ fallback preserves other modem bits without issuing BREAK', async () => {
  const calls = []
  const handle = { fd: 12, isOpen: true, set: async () => { throw unsupported() } }
  await setSerialSignals(handle, { dtr: true, rts: false }, { platform: 'linux', getIoctl: async () => (fd, request, value) => {
    calls.push([fd, request, value[0]])
    if (request === 0x5415) value[0] = 0x104
  } })
  assert.deepEqual(calls, [[12, 0x5415, 0], [12, 0x5418, 0x102]])
})
test('modem fallback does not conceal other errors or explicit BREAK failures', async () => {
  for (const [platform, brk, error] of [['win32', false, unsupported()], ['linux', true, unsupported()], ['linux', false, new Error('Device lost')]]) {
    await assert.rejects(setSerialSignals({ fd: 12, isOpen: true, set: async () => { throw error } }, { brk }, {
      platform, getIoctl: async () => { assert.fail('Unexpected fallback') },
    }), e => e === error)
  }
})
test('modem fallback refuses a descriptor closed while its backend loads', async () => {
  const handle = { fd: 12, isOpen: true, set: async () => { throw unsupported() } }
  await assert.rejects(setSerialSignals(handle, { dtr: true }, { platform: 'linux', getIoctl: async () => {
    handle.isOpen = false
    return () => assert.fail('Closed descriptor used')
  } }), /closed/)
})

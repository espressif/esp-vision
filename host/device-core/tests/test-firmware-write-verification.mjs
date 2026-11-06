// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { installFixture } from './firmware-fixtures.mjs'

async function fixture() {
  const f = await installFixture(), write = f.loader.flashData
  f.loader.flashData = async (...args) => {
    await write(...args)
    return { algorithm: 'md5', offset: args[2], bytes: args[0].byteLength,
      digest: createHash('md5').update(f.flash.subarray(args[2], args[2] + args[0].byteLength)).digest('hex') }
  }
  return f
}

test('verified official write skips all firmware and FFAT data readback', async () => {
  const f = await fixture(), progress = []
  f.loader.readFlash = async () => { throw new Error('No bulk readback in standard installation') }
  const result = await f.device.firmware.update({ ...f.request, onProgress: value => progress.push(value) })
  assert.equal(result.verified, true)
  assert.deepEqual(f.flash.subarray(0, f.data.length), f.data)
  assert.ok(f.flash.subarray(0x20000).every(value => value === 0xff))
  assert.deepEqual(f.calls, ['open', 'initialize', 'stub', 'capacity', ['erase', 0x20000, 0x21000], ['write', 0], 'reset', 'close'])
  assert.equal(progress.at(-1), 100)
  assert.deepEqual(progress, [...progress].sort((a, b) => a - b))
})

for (const change of ['algorithm', 'offset', 'bytes', 'digest', 'malformed']) {
  test(`invalid ${change} in write receipt never reports success, resets or retries`, async () => {
    const f = await fixture(), write = f.loader.flashData, progress = []
    f.loader.flashData = async (...args) => {
      const receipt = await write(...args)
      if (change === 'algorithm') receipt.algorithm = 'sha256'
      if (change === 'offset') receipt.offset += 4096
      if (change === 'bytes') receipt.bytes--
      if (change === 'digest') receipt.digest = '0'.repeat(32)
      if (change === 'malformed') return true
      return receipt
    }
    await assert.rejects(f.device.firmware.update({ ...f.request, onProgress: value => progress.push(value) }), /verification failed/)
    assert.equal(f.calls.filter(call => Array.isArray(call) && call[0] === 'write').length, 1)
    assert.ok(!f.calls.includes('reset'))
    assert.ok(!progress.includes(100))
    assert.equal(f.calls.at(-1), 'close')
  })
}

test('application update still verifies layout before using the standard MD5 receipt', async () => {
  const f = await fixture(), table = f.release.image.partitionTable
  f.flash.set(f.data.subarray(table.offset, table.offset + table.bytes), table.offset)
  const before = f.flash.slice(0x20000)
  await f.device.firmware.update({ ...f.request, mode: 'update', data: f.data.slice(0x10000, 0x10004) })
  assert.deepEqual(f.calls.filter(call => Array.isArray(call) && call[0] === 'read'), [['read', 0x8000]])
  assert.deepEqual(f.flash.subarray(0x20000), before)
})

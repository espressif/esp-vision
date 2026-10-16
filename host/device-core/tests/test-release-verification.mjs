// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { test } from 'node:test'
import { verifyPublishedIntegrity } from '../scripts/verify-registry.mjs'

const integrity = 'sha512-test-content'
const url = 'https://registry.npmjs.org/%40esp-vision%2Fdevice-core/0.2.0'
const published = value => new Response(JSON.stringify({ dist: { integrity: value } }))
function fixture(replies) {
  const calls = [], waits = [], reports = []
  return { calls, waits, reports, options: {
    async fetcher(address, options) {
      calls.push({ address, options })
      const reply = replies.shift()
      if (reply instanceof Error) throw reply
      assert(reply, 'Unexpected verification request')
      return reply
    },
    wait: async milliseconds => { waits.push(milliseconds) },
    report: message => reports.push(message),
  } }
}
test('new npm version becomes visible after stale 404 metadata; only uncached reads repeat', async () => {
  const f = fixture([new Response('', { status: 404 }), published(integrity)])
  await verifyPublishedIntegrity(url, integrity, f.options)
  assert.equal(f.calls.length, 2); assert.deepEqual(f.waits, [1000])
  for (const call of f.calls) {
    assert.equal(call.address, url); assert.equal(call.options.cache, 'no-store')
    assert.equal(call.options.headers['Cache-Control'], 'no-cache')
    assert.equal(call.options.method ?? 'GET', 'GET')
  }
})
test('verification retries rate limits, temporary registry errors and network failures', async () => {
  const f = fixture([new Response('', { status: 429 }), new Response('', { status: 503 }), new TypeError('network unavailable'), published(integrity)])
  await verifyPublishedIntegrity(url, integrity, f.options)
  assert.deepEqual(f.waits, [1000, 2000, 4000])
})
for (const status of [400, 401, 403]) test(`registry HTTP ${status} fails without retries`, async () => {
  const f = fixture([new Response('', { status })])
  await assert.rejects(verifyPublishedIntegrity(url, integrity, f.options), new RegExp(`HTTP ${status}`))
  assert.equal(f.calls.length, 1); assert.deepEqual(f.waits, [])
})
test('published integrity mismatch fails immediately', async () => {
  const f = fixture([published('sha512-other-content')])
  await assert.rejects(verifyPublishedIntegrity(url, integrity, f.options), /integrity mismatch/)
  assert.equal(f.calls.length, 1); assert.deepEqual(f.waits, [])
})
test('missing published integrity is not accepted', async () => {
  const f = fixture([new Response('{}')])
  await assert.rejects(verifyPublishedIntegrity(url, integrity, f.options), /integrity mismatch/)
  assert.deepEqual(f.waits, [])
})
test('persistent propagation failures stop after six reads with bounded backoff', async () => {
  const f = fixture(Array.from({ length: 6 }, () => new Response('', { status: 404 })))
  await assert.rejects(verifyPublishedIntegrity(url, integrity, f.options), /after 6 reads/)
  assert.equal(f.calls.length, 6); assert.deepEqual(f.waits, [1000, 2000, 4000, 8000, 16000])
})

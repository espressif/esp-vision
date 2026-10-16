// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { test } from 'node:test'
import { DeviceSession } from '../src/device_session.js'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { EvRepl } from '../src/evrepl.js'

function fixture(options = {}) {
  const requests = [], chunks = []; let session, began, finished
  let token
  const peer = new EvMuxParser({ filterFrame(metadata, bytes) {
    const payload = metadata.encoding === 'binary' ? bytes.slice() : JSON.parse(new TextDecoder().decode(bytes))
    requests.push({ metadata, payload })
    let reply = { ok: true }
    if (payload instanceof Uint8Array) {
      assert.equal(metadata.method, 'file.write'); assert.equal(metadata.encoding, 'binary')
      assert.equal(metadata.vmEpoch, 7); assert.equal(metadata.sessionId, 'a'.repeat(32))
      assert.equal(Buffer.from(payload.slice(0, 16)).toString('hex'), token)
      const offset = new DataView(payload.buffer).getUint32(16, true)
      assert.equal(offset, chunks.reduce((sum, chunk) => sum + chunk.length, 0))
      chunks.push(payload.slice(20)); reply.writtenBytes = offset + payload.length - 20
      if (options.badAck) reply.writtenBytes--
      if (options.abort) options.abort.abort()
    } else if (payload.action === 'begin') {
      began = payload; token = payload.uploadId; reply.uploadId = token; reply.chunkBytes = 16384
      if (options.abortBegin) options.abortBegin.abort()
      if (options.failBegin) { options.failBegin = false; throw new Error('begin response lost') }
    } else if (payload.action === 'commit') {
      finished = true; reply.bytes = chunks.reduce((sum, chunk) => sum + chunk.length, 0); reply.complete = true
      if (options.badCommit) reply.bytes++
      if (options.failCommit) reply = { ok: false, error: { code: 'HASH_MISMATCH' } }
    }
    session.feedBytes(makeEvMuxFrame({ channel: 'user.rpc', type: 'rsp', method: metadata.method, seq: metadata.seq,
      sessionId: 'a'.repeat(32), vmEpoch: 7, payload: JSON.stringify(reply) }))
    return false
  } })
  session = new DeviceSession({ transport: { async writeBytes(bytes) { peer.feedBytes(bytes) } }, EvMuxParser, makeEvMuxFrame, EvRepl })
  Object.assign(session, { state: 'ready', sessionId: 'a'.repeat(32), vmEpoch: 7, vmReady: true,
    capabilities: { features: ['file.write.binary', 'script.run'] } })
  session.limits.fileWriteChunkMax = 16384
  return { session, requests, chunks, get began() { return began }, get finished() { return finished } }
}
for (const size of [0, 1, 4096, 16384, 16385, 2 * 1024 * 1024]) {
  test(`binary upload ${size} bytes is bounded, exact, hashed and committed`, async () => {
    const f = fixture(), data = Uint8Array.from({ length: size }, (_, i) => i & 255), progress = []
    try {
      const result = await f.session.writeFile('/model.bin', data, { encoding: 'binary', onProgress: value => progress.push(value.writtenBytes) })
      assert.equal(result.complete, true); assert.equal(result.bytes, size)
      assert.deepEqual(Buffer.concat(f.chunks), Buffer.from(data)); assert.equal(f.finished, true)
      assert.equal(f.began.sha256, Buffer.from(await crypto.subtle.digest('SHA-256', data)).toString('hex'))
      assert.ok(f.chunks.every(chunk => chunk.length <= 16384)); assert.equal(f.chunks.length, Math.ceil(size / 16384))
      assert.equal(progress.at(-1), size)
      assert.ok(!f.requests.some(request => request.metadata.method === 'script.write'))
    } finally { f.session.close() }
  })
}
test('Python upload uses the same byte engine and requests execution only after commit', async () => {
  const f = fixture()
  try {
    await f.session.writeScript('/script.py', "print('中文')", { runAfterWrite: true })
    assert.deepEqual(Buffer.concat(f.chunks), Buffer.from("print('中文')"))
    assert.equal(f.requests.at(-1).metadata.method, 'script.run')
    assert.equal(f.requests.at(-2).payload.action, 'commit')
  } finally { f.session.close() }
})
for (const setting of ['badAck', 'badCommit', 'failCommit']) {
  test(`${setting} triggers abort cleanup without publishing success`, async () => {
    const f = fixture({ [setting]: true })
    try {
      await assert.rejects(f.session.writeFile('/x.bin', new Uint8Array(100)))
      assert.equal(f.requests.at(-1).payload.action, 'abort')
    } finally { f.session.close() }
  })
}
test('cancellation after begin still sends an independent abort', async () => {
  const controller = new AbortController(), f = fixture({ abort: controller })
  try {
    await assert.rejects(f.session.writeFile('/x.bin', new Uint8Array(32768), { signal: controller.signal }), { name: 'AbortError' })
    assert.equal(f.requests.at(-1).payload.action, 'abort')
  } finally { f.session.close() }
})
test('missing binary capability rejects without falling back to script.write', async () => {
  const f = fixture(); f.session.capabilities.features = ['script.write']
  try { await assert.rejects(f.session.writeFile('/x.bin', new Uint8Array(10)), { code: 'UNSUPPORTED_OPERATION' }); assert.equal(f.requests.length, 0) }
  finally { f.session.close() }
})
test('corrupted binary frames fail CRC before dispatch', () => {
  let calls = 0
  const parser = new EvMuxParser({ filterFrame() { calls++; return false } })
  const bytes = makeEvMuxFrame({ channel: 'user.rpc', type: 'req', method: 'file.write', encoding: 'binary', payload: new Uint8Array([0, 1, 0x1e, 0x1f, 255]) })
  parser.feedBytes(bytes); assert.equal(calls, 1)
  bytes[bytes.length - 2] ^= 1; parser.feedBytes(bytes); assert.equal(calls, 1)
})

test('cancellation during begin still aborts the client-known upload ID', async () => {
  const controller = new AbortController(), f = fixture({ abortBegin: controller })
  try {
    await assert.rejects(f.session.writeFile('/x.bin', new Uint8Array(100), { signal: controller.signal }), { name: 'AbortError' })
    assert.equal(f.requests.at(-1).payload.action, 'abort')
    assert.equal(f.requests.at(-1).payload.uploadId, f.began.uploadId)
    assert.equal(f.chunks.length, 0)
  } finally { f.session.close() }
})
test('a lost begin response still allows abort without knowing a server-generated ID', async () => {
  const f = fixture({ failBegin: true })
  try {
    await assert.rejects(f.session.writeFile('/x.bin', new Uint8Array(100)), /begin response lost/)
    assert.equal(f.requests.at(-1).payload.action, 'abort')
    assert.equal(f.requests.at(-1).payload.uploadId, f.began.uploadId)
  } finally { f.session.close() }
})

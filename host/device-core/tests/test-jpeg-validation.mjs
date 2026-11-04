// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { validateJpegImage } from '../src/jpeg_validate.js'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { DeviceSession } from '../src/device_session.js'
import { EvRepl } from '../src/evrepl.js'
import { DeviceActionRegistry } from '../src/device_actions.js'
import { bindSessionDeviceActions } from '../src/device_bindings.js'
import { JPEG_FIXTURES, jpegFixture } from './jpeg-fixtures.mjs'

const invalidImage = error => error.code === 'INVALID_IMAGE' && /^Invalid JPEG image: /.test(error.message)
const join = (...parts) => Uint8Array.from(parts.flatMap(part => Array.from(part)))
const change = (bytes, offset, value) => { const result = bytes.slice(); result[offset] = value; return result }
// Locate a known fixture's header segments only; this is not a JPEG validator.
function headers(bytes) {
  const result = []
  for (let start = 2; start < bytes.length;) {
    const marker = bytes[start + 1], length = bytes[start + 2] * 256 + bytes[start + 3]
    result.push({ marker, start, end: start + length + 2 })
    if (marker === 0xda) break
    start += length + 2
  }
  return result
}
const header = (bytes, marker) => headers(bytes).find(segment => segment.marker === marker)
const replace = (bytes, segment, replacement) => join(bytes.subarray(0, segment.start), replacement, bytes.subarray(segment.end))
const segment = (marker, content) => join([0xff, marker, (content.length + 2) >> 8, (content.length + 2) & 255], content)

test('self-created baseline and progressive JPEG fixtures pass structural and metadata checks', () => {
  for (const name of Object.keys(JPEG_FIXTURES)) {
    const image = jpegFixture(name)
    assert.deepEqual(validateJpegImage(image.data, image), { width: image.width, height: image.height })
  }
  const image = jpegFixture(), sos = header(image.data, 0xda)
  // Metadata markers and unused 16-bit tables are bounded but do not alter pixels.
  const unusedQuant = segment(0xdb, [0x13, ...Array.from({ length: 64 }, () => [0, 1]).flat()])
  const enriched = join(image.data.subarray(0, sos.start), segment(0xfe, [65, 66]), unusedQuant, image.data.subarray(sos.start))
  assert.deepEqual(validateJpegImage(enriched, image), { width: 2, height: 1 })
})

test('JPEG validation rejects fake images, every truncated prefix, trailing bytes and mismatched dimensions', () => {
  const image = jpegFixture()
  for (const bytes of [new Uint8Array(), Uint8Array.of(0xff, 0xd8, 0xff, 0xd9), join(image.data, [0]), change(image.data, 0, 0)]) {
    assert.throws(() => validateJpegImage(bytes, image), invalidImage)
  }
  for (let length = 0; length < image.data.length; length++) {
    assert.throws(() => validateJpegImage(image.data.subarray(0, length), image), invalidImage, `prefix ${length}`)
  }
  assert.throws(() => validateJpegImage(Array.from(image.data), image), invalidImage)
  for (const metadata of [{}, { width: '2', height: 1 }, { width: 2.5, height: 1 }, { width: 2, height: 0 }, { width: 2, height: 65536 }, { width: 3, height: 1 }]) {
    assert.throws(() => validateJpegImage(image.data, metadata), invalidImage)
  }
})

test('JPEG table, component and scan references fail closed within segment bounds', () => {
  const image = jpegFixture(), bytes = image.data
  const sof = header(bytes, 0xc0), dqt = header(bytes, 0xdb), dht = header(bytes, 0xc4), sos = header(bytes, 0xda)
  const oversized = change(change(bytes, dqt.start + 2, 0xff), dqt.start + 3, 0xff)
  const used16bitQuant = replace(bytes, dqt, segment(0xdb, [0x10, ...Array.from({ length: 64 }, () => [0, 1]).flat()]))
  const repeatedSof = join(bytes.subarray(0, sos.start), bytes.subarray(sof.start, sof.end), bytes.subarray(sos.start))
  const missingCoverage = replace(bytes, sos, segment(0xda, [1, bytes[sos.start + 5], bytes[sos.start + 6], 0, 63, 0]))
  const malformed = [
    oversized, change(bytes, dqt.start + 5, 0), used16bitQuant,
    change(bytes, dht.start + 4, 0x24), // unsupported Huffman selector
    change(bytes, dht.start + 5, 3), // more one-bit codes than available
    change(bytes, dht.start + 5, 2), // entirely occupied code space, reserved all-ones code
    change(bytes, sof.start + 1, 0xc3), // unsupported lossless frame
    change(bytes, sof.start + 4, 12), // unsupported sample precision
    change(bytes, sof.start + 13, bytes[sof.start + 10]), // repeated component id
    change(bytes, sof.start + 11, 0x50), // invalid sampling factor
    change(bytes, sof.start + 12, 3), // missing quantization table
    repeatedSof, missingCoverage,
    change(bytes, sos.start + 5, 99), // undefined component
    change(bytes, sos.start + 6, 0x33), // missing Huffman tables
    change(bytes, sos.end - 2, 62), // sequential scans must include coefficients 0..63
  ]
  for (const candidate of malformed) assert.throws(() => validateJpegImage(candidate, image), invalidImage)
})

test('entropy stuffing, restart order and every restart segment require bounded nonempty data', () => {
  const image = jpegFixture(), sos = header(image.data, 0xda)
  const prefix = image.data.subarray(0, sos.start), scanHeader = image.data.subarray(sos.start, sos.end)
  const dri = segment(0xdd, [0, 1]), eoi = [0xff, 0xd9]
  const malformed = [
    join(prefix, scanHeader, eoi),
    join(prefix, scanHeader, [1, 0xff, 0xff, 0, 2], eoi),
    join(prefix, scanHeader, [1, 0xff, 0xd0, 2], eoi), // no DRI
    join(prefix, dri, scanHeader, [0xff, 0xd0, 1], eoi), // empty first segment
    join(prefix, dri, scanHeader, [1, 0xff, 0xd0], eoi), // empty last segment
    join(prefix, dri, scanHeader, [1, 0xff, 0xd1, 2], eoi), // sequence must start at RST0
    join(prefix, dri, scanHeader, [1, 0xff, 0xd0, 2, 0xff, 0xd0, 3], eoi),
  ]
  for (const candidate of malformed) assert.throws(() => validateJpegImage(candidate, image), invalidImage)
  // These mutations demonstrate structural scope only; entropy is deliberately not decoded.
  assert.doesNotThrow(() => validateJpegImage(join(prefix, dri, scanHeader, [1, 0xff, 0xd0, 2, 0xff, 0xd1, 3], eoi), image))
  assert.doesNotThrow(() => validateJpegImage(join(prefix, scanHeader, [1, 0xff, 0, 2], eoi), image))
})

test('progressive scan layout and initial component coverage are validated without decoding coefficients', () => {
  const image = jpegFixture('progressive'), bytes = image.data, sos = header(bytes, 0xda)
  assert.throws(() => validateJpegImage(change(bytes, sos.end - 2, 63), image), invalidImage, 'a DC scan must end at coefficient zero')
  assert.throws(() => validateJpegImage(change(bytes, sos.end - 1, 0x31), image), invalidImage, 'successive approximation must progress by one bit')
  assert.throws(() => validateJpegImage(change(bytes, sos.start + 6, 0x30), image), invalidImage, 'initial DC scan needs its selected Huffman table')
  const noInitialDc = change(bytes, sos.end - 1, 0x10)
  assert.throws(() => validateJpegImage(noInitialDc, image), invalidImage, 'refinement alone does not establish initial component coverage')
})

function fixture(t) {
  const requests = [], events = { preview: [], debugImage: [], rpcResponse: [], diagnostic: [] }
  const recorder = new EvMuxParser({ filterFrame(metadata) { requests.push(metadata); return false } })
  const session = new DeviceSession({ transport: { async writeBytes(bytes) { recorder.feedBytes(bytes) } },
    handlers: Object.fromEntries(Object.keys(events).map(name => [name, (...args) => events[name].push(args)])), EvMuxParser, makeEvMuxFrame, EvRepl })
  session.state = 'ready'; session.sessionId = 'a'.repeat(32); session.vmEpoch = 1; session.vmReady = true
  session.capabilities = { features: ['debug.capture_frame', 'debug.capture_frame.observed.v1', 'debug.capture_screen'] }
  t.after(() => session.close())
  const imageFrame = (request, { image = jpegFixture(), payload = image.data, ...overrides } = {}) => makeEvMuxFrame({
    ...request, channel: request.channel ?? 'debug.rpc', type: 'rsp', method: request.method ?? 'debug.capture_frame',
    sessionId: session.sessionId, vmEpoch: 1, contentType: 'image/jpeg', encoding: 'binary',
    width: image.width, height: image.height, payload, ...overrides,
  })
  const jsonFrame = (request, payload, overrides = {}) => makeEvMuxFrame({ ...request, type: 'rsp', sessionId: session.sessionId, vmEpoch: 1, payload, ...overrides })
  return { session, requests, events, imageFrame, jsonFrame }
}
const tick = () => new Promise(setImmediate)
function feedFragments(session, bytes) {
  for (let cursor = 0, size = 1; cursor < bytes.length; size = size % 19 + 1) {
    session.feedBytes(bytes.subarray(cursor, cursor + size)); cursor += size
  }
}

test('CRC-valid malformed camera and screen JPEGs reject correlated captures immediately with no image callback', async t => {
  const f = fixture(t)
  for (const method of ['debug.capture_frame', 'debug.capture_screen']) {
    const pending = f.session.captureImage(method, {}, { timeout: 5000 })
    const rejected = assert.rejects(pending, invalidImage)
    await tick()
    feedFragments(f.session, f.imageFrame(f.requests.at(-1), { payload: Uint8Array.of(0xff, 0xd8, 0xff, 0xd9) }))
    await rejected
    assert.equal(f.session.pendingRpc.size, 0)
  }
  assert.equal(f.events.debugImage.length, 0); assert.equal(f.events.preview.length, 0); assert.equal(f.events.rpcResponse.length, 0)
  assert.equal(f.session.parser.getDiagnostics().invalidImageFrames, 2)
  assert.equal(f.session.parser.getDiagnostics().droppedFrames, 2)
  assert.equal(f.events.diagnostic.filter(([value]) => value.event === 'image.invalid').length, 2)
  const next = f.session.captureImage('debug.capture_frame'); await tick()
  feedFragments(f.session, f.imageFrame(f.requests.at(-1)))
  assert.deepEqual((await next).data, jpegFixture().data, 'a valid image recovers after rejected frames')
})

test('a CRC-damaged image is dropped before JPEG validation and does not consume the matching request', async t => {
  const f = fixture(t), pending = f.session.captureImage('debug.capture_frame'); await tick()
  const request = f.requests.at(-1), frame = f.imageFrame(request)
  frame[frame.length - jpegFixture().data.length - 1] = 0 // corrupt SOI, retaining the original frame CRC
  feedFragments(f.session, frame)
  assert.equal(f.session.pendingRpc.size, 1)
  assert.equal(f.session.parser.getDiagnostics().invalidImageFrames, 0)
  assert.equal(f.session.parser.getDiagnostics().droppedFrames, 1)
  assert.equal(f.events.debugImage.length, 0)
  f.session.feedBytes(f.imageFrame(request))
  assert.deepEqual((await pending).data, jpegFixture().data)
})

test('invalid previews drop a complete frame, retain pending requests and recover on fragmented valid preview', async t => {
  const f = fixture(t)
  const pending = f.session.rpc('debug.info', { scope: 'transport.stats' }); await tick()
  const request = f.requests.at(-1)
  for (const overrides of [{ payload: Uint8Array.of(0xff, 0xd8, 0xff, 0xd9) }, { width: 99 }, { encoding: 'text' }, { type: 'rsp' }]) {
    feedFragments(f.session, f.imageFrame({ channel: 'preview.frame', seq: request.seq }, { type: 'data', ...overrides }))
    assert.equal(f.session.pendingRpc.size, 1)
  }
  assert.equal(f.events.preview.length, 0); assert.equal(f.events.debugImage.length, 0)
  feedFragments(f.session, f.imageFrame({ channel: 'preview.frame', seq: request.seq }, { type: 'data' }))
  assert.equal(f.events.preview.length, 1)
  assert.deepEqual(f.events.preview[0][2], jpegFixture().data)
  const stats = f.session.parser.getDiagnostics()
  assert.equal(stats.invalidImageFrames, 4); assert.equal(stats.droppedFrames, 4); assert.equal(stats.frames, 1)
  f.session.feedBytes(f.jsonFrame(request, JSON.stringify({ ok: true, stats: {} })))
  assert.equal((await pending).ok, true)
})

test('capture JSON success, malformed JSON and nonbinary JPEG reject while firmware capture errors remain explicit', async t => {
  const f = fixture(t)
  for (const response of [
    request => f.jsonFrame(request, JSON.stringify({ ok: true })),
    request => f.jsonFrame(request, '{'),
    request => f.jsonFrame(request, JSON.stringify({ ok: false })),
    request => f.imageFrame(request, { encoding: 'json' }),
    request => f.imageFrame(request, { contentType: 'image/png' }),
    request => f.jsonFrame(request, JSON.stringify({ ok: false, error: { code: 'NOT_OWNER' } }), { contentType: 'image/jpeg', sessionId: undefined, vmEpoch: undefined }),
  ]) {
    // Even lower-level generic RPC callers cannot accept a JPEG-less capture success.
    const pending = f.session.rpc('debug.capture_frame', {}, { timeout: 5000 })
    const rejected = assert.rejects(pending, invalidImage); await tick()
    f.session.feedBytes(response(f.requests.at(-1))); await rejected
  }
  const pending = f.session.captureImage('debug.capture_screen'); const rejected = assert.rejects(pending, { code: 'JPEG_FAILED' }); await tick()
  f.session.feedBytes(f.jsonFrame(f.requests.at(-1), JSON.stringify({ ok: false, error: { code: 'JPEG_FAILED', message: 'Encoding failed' } })))
  await rejected
  assert.equal(f.events.debugImage.length, 0); assert.equal(f.events.rpcResponse.length, 1)
})

test('malformed stale image responses cannot reject captures or unrelated RPCs', async t => {
  const f = fixture(t), pending = f.session.captureImage('debug.capture_frame'), query = f.session.rpc('debug.info', { scope: 'transport.stats' })
  await tick()
  const capture = f.requests.find(request => request.method === 'debug.capture_frame'), info = f.requests.find(request => request.method === 'debug.info')
  for (const overrides of [{ seq: capture.seq + 100 }, { sessionId: 'b'.repeat(32) }, { vmEpoch: 2 }, { method: 'debug.capture_screen' }, { seq: info.seq }]) {
    f.session.feedBytes(f.imageFrame(capture, { payload: Uint8Array.of(0xff, 0xd8, 0xff, 0xd9), ...overrides }))
    f.session.feedBytes(f.jsonFrame({ ...capture, ...overrides }, JSON.stringify({ ok: true }), overrides))
    assert.equal(f.session.pendingRpc.size, 2)
  }
  assert.equal(f.session.parser.getDiagnostics().invalidImageFrames, 0, 'correlation rejects stale responses before JPEG validation')
  assert.equal(f.events.debugImage.length, 0)
  f.session.feedBytes(f.jsonFrame(info, JSON.stringify({ ok: true, stats: { marker: 7 } })))
  f.session.feedBytes(f.imageFrame(capture))
  assert.equal((await query).stats.marker, 7); assert.deepEqual((await pending).data, jpegFixture().data)
})

test('frame.capture action returns its own binary image when concurrent responses arrive in reverse order', async t => {
  const f = fixture(t), registry = new DeviceActionRegistry(); bindSessionDeviceActions(registry)
  const capture = registry.handler('frame.capture')
  const first = capture({ timeoutMs: 5000 }, { session: f.session }), second = capture({ timeoutMs: 5000 }, { session: f.session })
  await tick()
  const firstRequest = f.requests[0], secondRequest = f.requests[1]
  f.session.feedBytes(f.imageFrame(secondRequest, { image: jpegFixture('small') }))
  f.session.feedBytes(f.imageFrame(firstRequest, { image: jpegFixture('alternate') }))
  const [a, b] = await Promise.all([first, second])
  assert.equal(a.base64, JPEG_FIXTURES.alternate.base64); assert.equal(a.metadata.seq, firstRequest.seq)
  assert.equal(a.metadata.width, 2); assert.equal(a.metadata.height, 1)
  assert.equal(b.base64, JPEG_FIXTURES.small.base64); assert.equal(b.metadata.seq, secondRequest.seq)
  assert.equal(b.metadata.width, 4); assert.equal(b.metadata.height, 3)
})

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { createDevice } from '../src/index.js'
import { DeviceSession, UART_BAUD_CANDIDATES, EV_MUX_DEFAULT_LIMITS, EV_MUX_REQUIRED_CHANNELS, EV_MUX_REQUIRED_FEATURES } from '../src/device_session.js'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { EvRepl } from '../src/evrepl.js'

function peer({ owner = 'uart', support = true, rejectSpeeds = [], corruptSpeeds = [], corruptFrames = [], silentSpeeds = [], dropCommit = false, lostPrepare = false, lostCommitAndRelease = false, resetOnChange = false } = {}) {
  let listener, session, hostBaud = 115200, deviceBaud = 115200, active = false, token, commitDrops = 0
  const id = 'a'.repeat(32), requests = [], changed = [], diagnostics = [], sleeps = []
  const caps = { ok: true, protocol: { evMuxVersion: 4 }, features: EV_MUX_REQUIRED_FEATURES,
    channels: EV_MUX_REQUIRED_CHANNELS, limits: { ...EV_MUX_DEFAULT_LIMITS, txLargePayloadMax: 262144 },
    session: { policy: 'firstCome', leaseMs: 15000, keepaliveMs: 5000 },
    ...(support ? { uartBaud: { baseline: 115200, current: 115200, candidates: UART_BAUD_CANDIDATES, probeBytes: 512, timeoutMs: 5000, switchDelayMs: 100 } } : {}),
  }
  const reset = () => { active = false; deviceBaud = 115200; token = null }
  const reader = new EvMuxParser({ filterFrame(metadata, bytes) {
    if (hostBaud !== deviceBaud) return false
    const payload = JSON.parse(new TextDecoder().decode(bytes))
    requests.push({ method: metadata.method, payload, baudRate: hostBaud, state: session?.state })
    let result = { ok: true }, acquired = false
    if (metadata.method === 'hello') {
      if (payload.acquire) { active = true; acquired = true }
      result = { ok: true, evMuxVersion: 4, deviceId: 'test', hostname: 'test', vmReady: false,
        session: active ? { id, owner, vmEpoch: 1, leaseMs: 15000, keepaliveMs: 5000 } : null }
    } else if (metadata.method === 'capabilities') result = caps
    else if (metadata.method === 'transport.uart.prepare') {
      token = payload.token
      if (lostPrepare && payload.baudRate === 2000000) { deviceBaud = payload.baudRate; return false }
      result = { ok: true, ...payload, timeoutMs: 5000, switchDelayMs: 100 }
    } else if (metadata.method === 'transport.uart.probe') {
      if (silentSpeeds.includes(hostBaud)) return false
      result = { ok: true, token, data: corruptSpeeds.includes(hostBaud) ? payload.data + 'x' : payload.data }
    } else if (metadata.method === 'transport.uart.commit') {
      if (lostCommitAndRelease && hostBaud === 2000000) return false
      if (dropCommit && commitDrops++ === 0) return false
      result = { ok: true, token, baudRate: deviceBaud }
    }
    if (metadata.method === 'session.release' && lostCommitAndRelease && hostBaud === 2000000) return false
    const response = makeEvMuxFrame({ channel: metadata.channel, type: 'rsp', method: metadata.method, seq: metadata.seq,
      ...((acquired || !['hello', 'capabilities'].includes(metadata.method)) ? { sessionId: id, vmEpoch: 1 } : {}), payload: JSON.stringify(result) })
    if (metadata.method === 'transport.uart.probe' && corruptFrames.includes(hostBaud)) response[response.length - 2] ^= 1
    listener.data(response)
    if (metadata.method === 'transport.uart.prepare') deviceBaud = payload.baudRate
    if (metadata.method === 'session.release') reset()
    return false
  } })
  const transport = {
    subscribe(value) { listener = value; return () => { listener = null } },
    async connect() { hostBaud = 115200 },
    async disconnect() {},
    async writeBytes(bytes) { reader.feedBytes(bytes) },
    async setBaudRate(speed) {
      changed.push(speed)
      if (rejectSpeeds.includes(speed)) throw Object.assign(new Error('unsupported speed'), { code: 'SERIAL_BAUD_UNSUPPORTED' })
      hostBaud = speed
      if (resetOnChange && speed !== 115200) reset()
    },
  }
  const sleep = async ms => { sleeps.push(ms); if (ms >= 1000) reset() }
  session = new DeviceSession({ transport, EvMuxParser, makeEvMuxFrame, EvRepl, sleep, handlers: { diagnostic: event => diagnostics.push(event) } })
  listener = { data: bytes => session.feedBytes(bytes) }
  return { transport, session, requests, changed, diagnostics, sleeps }
}

test('runtime tests 2 Mbps first with varied bidirectional probes and stops after success', async () => {
  const p = peer()
  try {
    await p.session.negotiate({ drainMs: 0, probeRepl: false })
    assert.deepEqual(p.changed, [2000000])
    assert.deepEqual(p.requests.filter(x => x.method === 'transport.uart.probe').map(x => x.payload.data.length), [32, 32, 128, 128, 512, 512])
    assert.equal(p.requests.filter(x => x.method === 'transport.uart.commit').length, 1)
    assert.equal(p.session.capabilities.uartBaud.current, 2000000)
    assert.ok(p.requests.every(x => x.state !== 'ready'), 'no request or event can observe ready before verification')
    assert.equal(p.session.state, 'ready')
  } finally { p.session.close() }
})

test('corrupt echoes and adapter rejection recover at baseline and select first passing lower speed', async () => {
  const p = peer({ corruptSpeeds: [2000000], rejectSpeeds: [1500000] })
  try {
    await p.session.negotiate({ drainMs: 0, probeRepl: false })
    assert.deepEqual(p.changed, [2000000, 115200, 1500000, 115200, 921600])
    assert.deepEqual(p.requests.filter(x => x.method === 'transport.uart.prepare').map(x => x.payload.baudRate), [2000000, 1500000, 921600])
    assert.equal(p.requests.filter(x => x.method === 'hello' && x.payload.acquire).length, 3)
    assert.equal(p.session.capabilities.uartBaud.current, 921600)
  } finally { p.session.close() }
})

test('all high speeds can fail while retaining a fully verified 115200 connection', async () => {
  const p = peer({ corruptSpeeds: UART_BAUD_CANDIDATES.slice(0, -1) })
  try {
    await p.session.negotiate({ drainMs: 0, probeRepl: false })
    assert.deepEqual(p.requests.filter(x => x.method === 'transport.uart.prepare').map(x => x.payload.baudRate), UART_BAUD_CANDIDATES)
    assert.equal(p.session.capabilities.uartBaud.current, 115200)
    assert.equal(p.session.state, 'ready')
  } finally { p.session.close() }
})

test('a timed out new-rate probe and a reopen-triggered reset both recover before the next candidate', async () => {
  for (const options of [{ silentSpeeds: [2000000] }, { corruptFrames: [2000000] }, { resetOnChange: true }]) {
    const p = peer(options)
    try {
      await p.session.negotiate({ drainMs: 0, probeRepl: false })
      assert.equal(p.session.capabilities.uartBaud.current, options.resetOnChange ? 115200 : 1500000)
      assert.ok(p.sleeps.some(ms => ms >= (options.resetOnChange ? 1000 : 150)), 'wait for release completion or independent rollback before reacquiring')
    } finally { p.session.close() }
  }
})

test('a lost commit acknowledgement retries the idempotent token without downgrading', async () => {
  const p = peer({ dropCommit: true })
  try {
    await p.session.negotiate({ drainMs: 0, probeRepl: false })
    assert.deepEqual(p.changed, [2000000])
    const commits = p.requests.filter(x => x.method === 'transport.uart.commit')
    assert.equal(commits.length, 2)
    assert.equal(commits[0].payload.token, commits[1].payload.token)
  } finally { p.session.close() }
})

test('lost prepare and committed-session ACKs wait full rollback bounds after the last failed release', async () => {
  for (const options of [{ lostPrepare: true }, { lostCommitAndRelease: true }]) {
    const p = peer(options)
    try {
      await p.session.negotiate({ drainMs: 0, probeRepl: false })
      assert.ok(p.sleeps.includes(options.lostPrepare ? 5250 : 15250), 'prepare delay and lease renewal must not shorten the recovery window')
      assert.equal(p.session.capabilities.uartBaud.current, 1500000)
    } finally { p.session.close() }
  }
})

test('a timed out adapter retune receives cancellation before baseline recovery', async () => {
  const p = peer(), original = p.transport.setBaudRate
  let aborted = false
  p.transport.setBaudRate = (speed, options) => speed !== 2000000 ? original(speed, options) : new Promise((_, reject) => {
    options.signal.addEventListener('abort', () => { aborted = true; reject(options.signal.reason) }, { once: true })
  })
  try {
    await p.session.negotiate({ drainMs: 0, probeRepl: false })
    assert.equal(aborted, true)
    assert.equal(p.session.capabilities.uartBaud.current, 1500000)
  } finally { p.session.close() }
})

test('USB owners, old firmware and transports lacking a retune method keep baseline', async () => {
  for (const options of [{ owner: 'usb' }, { owner: 'usj' }, { owner: 'tcp' }, { support: false }, { noAdapter: true }]) {
    const p = peer(options)
    if (options.noAdapter) delete p.transport.setBaudRate
    try {
      await p.session.negotiate({ drainMs: 0, probeRepl: false })
      assert.deepEqual(p.changed, [])
      assert.ok(!p.requests.some(x => x.method.startsWith('transport.uart.')))
    } finally { p.session.close() }
  }
})

test('public connect publishes ready only after runtime speed selection', async () => {
  const p = peer(), events = []
  const device = createDevice({ transport: p.transport })
  device.on('state', event => events.push(event.state))
  device.on('diagnostic', event => { if (event.event === 'uart.baud.selected') events.push('selected') })
  try {
    await device.connect({ probeRepl: false })
    assert.ok(events.indexOf('selected') >= 0)
    assert.ok(events.indexOf('ready') > events.indexOf('selected'))
  } finally { await device.disconnect() }
})

test('public passive identity probes can explicitly disable retuning', async () => {
  const p = peer(), device = createDevice({ transport: p.transport })
  for (const autoBaud of [null, 'false', 0]) await assert.rejects(device.connect({ autoBaud }), { name: 'TypeError' })
  try {
    await device.connect({ probeRepl: false, autoBaud: false })
    assert.deepEqual(p.changed, [])
    assert.ok(!p.requests.some(x => x.method.startsWith('transport.uart.')))
  } finally { await device.disconnect() }
})

// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { createPythonApi } from '../src/device_python.js'
import { createFilesApi } from '../src/device_files.js'

function fixture() {
  const calls = [], state = {}
  const session = {
    state: 'ready',
    repl: { writeStdin: async data => calls.push(['input', data]), removeFile: async path => calls.push(['remove', path]) },
    executePython: async (source, options) => { calls.push(['source', source, options]); return '42\r\n' },
    runScript: async (path, content, options) => { calls.push(['run', path, content, options]); return { ok: true, output: 'existing', outputTruncated: false } },
    writeFile: async () => { throw new Error('Unexpected write') },
  }
  return { session, state, calls, python: createPythonApi({ session, state }) }
}

test('source wait and path wait return the same result shape without rewriting path', async () => {
  const { python, calls } = fixture()
  assert.deepEqual(await python.execute({ source: 'print(42)' }), { ok: true, path: null, output: '42\n', outputTruncated: false })
  assert.deepEqual(await python.execute({ path: '/existing.py' }), { ok: true, path: '/existing.py', output: 'existing', outputTruncated: false })
  assert.equal(calls[0][2].timeout, null, 'wait execution has no aggregate deadline by default')
  assert.deepEqual(calls[1].slice(0, 3), ['run', '/existing.py', undefined])
  assert.equal(calls[1][3].timeout, null, 'path execution has no aggregate deadline by default')
  assert.equal(calls[1][3].inlineCompletion, true)
})

test('execution rejects ambiguous input, unsupported modes, unsafe paths and invalid deadlines before I/O', async () => {
  const { python, calls } = fixture()
  for (const request of [{}, { source: '', path: '/a.py' }, { source: 0 }, { path: '../a.py' }, { path: '/a/../b.py' }, { source: '', completion: 'other' }, { source: '', timeout: 0 }, { source: '', completion: 'start', timeout: 3 }, { source: '', completion: 'start', timeout: null }]) {
    await assert.rejects(python.execute(request))
  }
  assert.equal(calls.length, 0)
})

test('wait execution accepts null and forwards an explicit positive deadline', async () => {
  const { python, calls } = fixture()
  await python.execute({ source: 'print(42)', timeout: null })
  await python.execute({ source: 'print(42)', timeout: 12000 })
  assert.equal(calls[0][2].timeout, null)
  assert.equal(calls[1][2].timeout, 12000)
})

test('pre-aborted execution never starts Python', async () => {
  const { python, calls } = fixture()
  await assert.rejects(python.execute({ source: '', signal: AbortSignal.abort() }), { name: 'AbortError' })
  assert.equal(calls.length, 0)
})

test('existing script start preserves the file and permits terminal input while rejecting parallel work', async () => {
  const { python, calls, session, state } = fixture()
  let finish
  session.startScript = async (path, content, options) => {
    assert.equal(options.inlineCompletion, true)
    calls.push(['start', path, content])
    return { done: new Promise(resolve => { finish = resolve }) }
  }
  const handle = await python.execute({ path: '/model-demo.py', completion: 'start' })
  assert.deepEqual(calls[0], ['start', '/model-demo.py', undefined])
  assert.equal(state.run.status, 'running')
  await python.input('hello\r')
  await assert.rejects(python.execute({ source: 'pass' }), { code: 'DEVICE_BUSY' })
  const files = createFilesApi({ session, state })
  await assert.rejects(files.write({ path: '/other', data: 'x' }), { code: 'DEVICE_BUSY' })
  finish({ ok: true, output: '', outputTruncated: false })
  await handle.done
  assert.equal(state.run, null)
  assert.equal(state.operation, null)
  assert.ok(!calls.some(call => call[0] === 'remove'))
})

test('started source uses a private temporary file and removes it only after completion', async () => {
  const { python, calls, session } = fixture()
  let finish
  session.startScript = async (path, content, options) => {
    assert.equal(options.inlineCompletion, true)
    calls.push(['start', path, content])
    return { done: new Promise(resolve => { finish = resolve }) }
  }
  const handle = await python.execute({ source: 'while True: pass', completion: 'start' })
  assert.match(handle.path, /^\/\.__evcore_.*\.py$/)
  assert.equal(calls.length, 1)
  assert.equal(calls[0][2], 'while True: pass')
  finish({ ok: true, output: 'done' })
  assert.equal((await handle.done).output, 'done')
  assert.deepEqual(calls.at(-1), ['remove', handle.path])
})

test('disconnect skips remote cleanup for a started temporary source', async () => {
  const { python, calls, session, state } = fixture()
  let rejectDone
  session.stopForRepl = async () => { calls.push(['stop']); return true }
  session.startScript = async () => ({ done: new Promise((_resolve, reject) => { rejectDone = reject }) })
  const handle = await python.execute({ source: 'while True: pass', completion: 'start' })
  const disconnected = assert.rejects(handle.done, /device disconnected/)
  session.state = 'disconnected'
  rejectDone(new Error('device disconnected'))
  await disconnected
  assert.equal(calls.some(([kind]) => kind === 'stop' || kind === 'remove'), false)
  assert.equal(state.operation, null)
  assert.equal(state.run, null)
})

test('execution failures release the operation and retain the original error', async () => {
  const { python, session, state } = fixture()
  const failure = Object.assign(new Error('disconnected'), { code: 'SESSION_REQUIRED' })
  session.startScript = async () => { throw failure }
  session.repl.removeFile = async () => { throw new Error('cleanup also failed') }
  await assert.rejects(python.execute({ source: 'pass', completion: 'start' }), error => error === failure)
  assert.equal(state.run, null)
  assert.equal(state.operation, null)
})

test('terminal input is excluded during startup and automated execution', async () => {
  const { python, session } = fixture()
  let finish
  session.executePython = async () => new Promise(resolve => { finish = resolve })
  const execution = python.execute({ source: 'pass' })
  await assert.rejects(python.input('oops'), { code: 'DEVICE_BUSY' })
  finish('')
  await execution
  await python.input(new Uint8Array([13]))
  await assert.rejects(python.input({}), TypeError)
})


test('source execution honors the same output bound as path execution', async () => {
  const { python } = fixture()
  assert.deepEqual(await python.execute({ source: 'print(42)', maxOutputChars: 1 }), { ok: true, path: null, output: '4', outputTruncated: true })
})

test('interactive script input receives only user data, never the completion marker command', async () => {
  const { DeviceSession } = await import('../src/device_session.js')
  const { EvMuxParser, makeEvMuxFrame } = await import('../src/evmux.js')
  const { EvRepl } = await import('../src/evrepl.js')
  const input = [], methods = []
  let session, waitingForInput = false
  const stdout = payload => session.feedBytes(makeEvMuxFrame({
    channel: 'repl.stdout', type: 'data', sessionId: session.sessionId, vmEpoch: session.vmEpoch, payload,
  }))
  const parser = new EvMuxParser({ filterFrame(metadata, bytes) {
    const text = new TextDecoder().decode(bytes)
    if (metadata.channel === 'repl.signal') return
    if (metadata.channel === 'repl.stdin') {
      if (waitingForInput) {
        input.push(text)
        waitingForInput = false
        stdout(`${text}\r\nreceived:${text.trim()}\r\n${session.repl.scriptEndMarker}\r\n>>> `)
      } else if (text.includes('/interactive.py')) {
        waitingForInput = true
        const startMarker = /~~EVSCRIPT-START:[A-Za-z0-9-]+~~/.exec(text)?.[0]
        stdout(`${text}\r\n${startMarker ? `${startMarker}\r\n` : ''}NAME?`)
      } else stdout(`${text}\r\n>>> `)
      return
    }
    methods.push(metadata.method)
    // Legacy script.run deliberately models input() consuming the next stdin line.
    if (metadata.method === 'script.run') waitingForInput = true
    session.feedBytes(makeEvMuxFrame({ channel: metadata.channel, type: 'rsp', method: metadata.method, seq: metadata.seq,
      sessionId: session.sessionId, vmEpoch: session.vmEpoch, payload: JSON.stringify({ ok: true }),
    }))
  } })
  session = new DeviceSession({ transport: { async writeBytes(bytes) { parser.feedBytes(bytes) } }, EvMuxParser, makeEvMuxFrame, EvRepl })
  session.state = 'ready'; session.sessionId = 'a'.repeat(32); session.vmEpoch = 1; session.vmReady = true
  session.capabilities = { features: ['script.run', 'device.control'] }
  try {
    const python = createPythonApi({ session, state: {} })
    const run = await python.execute({ path: '/interactive.py', completion: 'start', maxOutputChars: 1024 })
    assert.deepEqual(input, [], 'Core must not pre-fill the application input with its private completion command')
    assert.equal(waitingForInput, true)
    await python.input('ev-api-check\r')
    const result = await run.done
    assert.deepEqual(input, ['ev-api-check\r'])
    assert.match(result.output, /received:ev-api-check/)
    assert.ok(!methods.includes('script.write'), 'Existing scripts must never be overwritten')
  } finally { session.close() }
})

test('Python requests reject unknown parameters and malformed signals before interpreter I/O', async () => {
  const { python, calls } = fixture()
  for (const signal of [{}, true, 'signal', { aborted: false, addEventListener() {}, removeEventListener() {} }]) {
    await assert.rejects(python.execute({ source: 'pass', signal }), TypeError)
  }
  await assert.rejects(python.execute({ source: 'pass', mode: 'wait' }), /Unknown Python execution parameter/)
  await assert.rejects(python.execute({ source: 'pass', [Symbol('option')]: true }), /Unknown Python execution parameter/)
  assert.equal(calls.length, 0)
})

test('Python snapshots request getters and input byte buffers', async () => {
  const { python, session } = fixture()
  let reads = 0
  await python.execute({ get source() { reads++; return reads === 1 ? 'safe' : 'changed' } })
  assert.equal(reads, 1)
  let release, observed
  session.repl.writeStdin = async data => {
    await new Promise(resolve => { release = resolve })
    observed = Array.from(data)
  }
  const data = new Uint8Array([65])
  const writing = python.input(data)
  data[0] = 66
  release()
  await writing
  assert.deepEqual(observed, [65])
})

test('disconnect and cleanup errors retain execution failure and always release shared state', async () => {
  const { python, session, state } = fixture()
  const original = Object.assign(new Error('transport closed during execution'), { code: 'SESSION_REQUIRED' })
  let rejectDone
  session.startScript = async () => ({ done: new Promise((_resolve, reject) => { rejectDone = reject }) })
  session.stopForRepl = async () => { throw new Error('cleanup transport closed') }
  const run = await python.execute({ source: 'pass', completion: 'start' })
  const failed = assert.rejects(run.done, error => error === original)
  rejectDone(original)
  await failed
  assert.equal(state.operation, null)
  assert.equal(state.run, null)
  assert.equal(await python.execute({ source: '1' }).then(result => result.ok), true)
})

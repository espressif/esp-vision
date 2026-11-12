// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { spawn, spawnSync } from 'node:child_process'
import { once } from 'node:events'
import { createInterface } from 'node:readline'
import { readFile } from 'node:fs/promises'
import { loadFixture } from './fixture-loader.mjs'
import { runDeviceChecks } from './device-checks.mjs'
import { parseArgs } from './hardware-check.mjs'
import { evReplExecInputBytes } from '../src/evrepl.js'

const python = [process.env.PYTHON, 'python3', 'python'].filter(Boolean).find(command => spawnSync(command, ['-c', 'pass'], { windowsHide: true }).status === 0)
const pythonTest = (name, callback) => test(name, { skip: !python && 'Python required for executable fixture protocol tests', timeout: 30000 }, callback)
const nonce = '0123456789abcdef', namespace = `_evhil_${nonce}`
const fixture = `runs=globals().get('runs',0)+1
def run_check(*args): pass
def close_display(): pass
def close_model(): pass
def model_command(*args): pass
# UTF-8 and escaped source: 测试 é \\ " '
`
const timeout = () => Object.assign(new Error('Injected lost response'), { code: 'TIMEOUT' })

async function peer(t, limit = 511) {
  // Execute the generated Python, including its state transitions, rather than
  // reproducing the receiver in a JS mock. Like the public SDK, a printed
  // Python exception can still return ok=true from the execution wrapper.
  const child = spawn(python, ['-u', '-c', `
import contextlib, io, json, sys, traceback, types
sys.modules['vfs'] = types.SimpleNamespace(mount=lambda: [], umount=lambda _: None)
scope = {}
for line in sys.stdin:
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        try:
            exec(json.loads(line), scope)
        except BaseException:
            traceback.print_exc(file=output)
    print(json.dumps({'ok': True, 'output': output.getvalue()}), flush=True)
`], { stdio: ['pipe', 'pipe', 'inherit'], windowsHide: true })
  const lines = createInterface({ input: child.stdout })[Symbol.asyncIterator]()
  const exited = once(child, 'exit')
  t.after(async () => { child.stdin.end(); await exited })
  const calls = [], reports = []
  const execute = async request => {
    assert.ok(evReplExecInputBytes(request.source) <= limit, `Command exceeds ${limit} bytes`)
    calls.push(request)
    child.stdin.write(JSON.stringify(request.source) + '\n')
    const line = await lines.next()
    assert.equal(line.done, false, 'Python peer exited')
    return JSON.parse(line.value)
  }
  const device = {
    async inspect() { return { identity: { board: 'TEST', firmwareVersion: 'test' }, capabilities: { features: [], protocol: { evMuxVersion: 4 }, limits: { replInputMax: limit } }, runtime: { vmReady: true } } },
    python: { execute },
  }
  const run = (source = fixture, signal) => loadFixture(source, { namespace, nonce, inputLimit: limit, execute: async source => (await device.python.execute({ source, signal })).output, signal, report: value => reports.push(value) })
  const inspect = async expression => JSON.parse((await execute({ source: `print(__import__('json').dumps(${expression}))` })).output)
  return { device, execute, calls, reports, run, inspect }
}

pythonTest('real fixture installs within both REPL limits, validates content and releases source memory', async t => {
  const source = await readFile(new URL('./fixtures/device-checks.py', import.meta.url), 'utf8')
  for (const limit of [256, 511]) {
    const p = await peer(t, limit)
    await p.run(source)
    assert.equal(p.reports.at(-1).event, 'ready')
    assert.equal(p.reports.at(-1).bytes, Buffer.byteLength(source))
    assert.deepEqual(await p.inspect(`[${namespace}['_state'],len(${namespace}['_source']),'_text' in ${namespace}]`), [2, 0, false])
  }
})

pythonTest('bootstrap recovers lost responses before and after committing each phase without duplicate installation', async t => {
  for (const phase of ['init', 'chunk', 'assemble', 'verify', 'install']) for (const committed of [false, true]) {
    const p = await peer(t)
    let injected = false
    p.device.python.execute = async request => {
      const matches = phase === 'init' ? request.source.startsWith(namespace + '=') : phase === 'install' ? request.source.includes('exec(_text,globals())') : request.source.includes(`'${phase}',`)
      if (matches && !injected) {
        injected = true
        if (committed) await p.execute(request)
        throw timeout()
      }
      return p.execute(request)
    }
    await p.run()
    assert.equal(injected, true, phase)
    assert.equal(await p.inspect(`${namespace}['runs']`), 1)
    assert.equal(p.reports.filter(r => r.event === 'retry').length, 1)
    assert.equal(p.reports.at(-1).retries, 1)
  }
})

pythonTest('identical duplicate chunks are idempotent and conflicting chunks cannot acknowledge success', async t => {
  for (const conflict of [false, true]) {
    const p = await peer(t)
    let injected = false
    p.device.python.execute = async request => {
      if (!injected && request.source.includes('_source.setdefault(0,')) {
        injected = true
        if (conflict) await p.execute({ source: `${namespace}['_source'][0]='conflict'` })
        else await p.execute(request)
      }
      return p.execute(request)
    }
    if (conflict) await assert.rejects(p.run(), { code: 'FIXTURE_ACK_INVALID' })
    else { await p.run(); assert.equal(await p.inspect(`${namespace}['runs']`), 1) }
    assert.equal(p.reports.filter(r => r.event === 'retry').length, 0)
  }
})

pythonTest('fixture ACK rejects stale sessions, wrong offsets, mismatched content and malformed output without retry', async t => {
  for (const fault of ['nonce', 'offset', 'content', 'duplicate', 'malformed', 'exception']) {
    const p = await peer(t)
    let attempts = 0
    p.device.python.execute = async request => {
      const response = await p.execute(request)
      if (request.source.includes('_source.setdefault(')) {
        attempts++
        const ack = JSON.parse(response.output.slice('__EVHILFIXTURE__'.length))
        if (fault === 'nonce') ack[0] = 'stale'
        if (fault === 'offset') ack[2][0]++
        if (fault === 'content') ack[2][1] += 'corrupt'
        response.output = '__EVHILFIXTURE__' + JSON.stringify(ack)
        if (fault === 'duplicate') response.output += '\n' + response.output
        if (fault === 'malformed') response.output = '__EVHILFIXTURE__bad'
        if (fault === 'exception') response.output = 'Traceback: OSError: no module named sensor'
      }
      return response
    }
    await assert.rejects(p.run(), { code: 'FIXTURE_ACK_INVALID' })
    assert.equal(attempts, 1)
    assert.equal(p.reports.length, 0)
  }
})

pythonTest('whole-source corruption and incomplete installation cannot produce a ready acknowledgement', async t => {
  for (const fault of ['hash', 'missing-function', 'partial-install']) {
    const p = await peer(t)
    let injected = false
    p.device.python.execute = async request => {
      if (fault === 'hash' && request.source.includes("_text=''.join") && !injected) {
        injected = true
        // Keep the length unchanged so only the SHA-256 gate detects this.
        await p.execute({ source: `${namespace}['_source'][0]='x'+${namespace}['_source'][0][1:]` })
      }
      const result = await p.execute(request)
      if (fault === 'partial-install' && request.source.includes('exec(_text,globals())') && !injected) {
        injected = true
        throw timeout()
      }
      return result
    }
    const source = fault === 'missing-function' ? fixture.replace('def model_command(*args): pass', '') : fault === 'partial-install' ? fixture + '\nraise KeyboardInterrupt()\n' : fixture
    await assert.rejects(p.run(source), error => error.code === 'FIXTURE_ACK_INVALID' && error.bootstrap.phase === (fault === 'hash' ? 'verify' : 'install'))
    assert.equal(p.reports.some(r => r.event === 'ready'), false)
    if (fault === 'partial-install') {
      assert.equal(await p.inspect(`${namespace}['runs']`), 1)
      assert.equal(await p.inspect(`${namespace}['_state']`), 1)
    }
  }
})

pythonTest('later chunk failures retain the last acknowledged offset and do not retry non-timeout errors', async t => {
  for (const code of ['TIMEOUT', 'DEVICE_BUSY', 'DISCONNECTED']) {
    const p = await peer(t, 256)
    let attempts = 0
    p.device.python.execute = async request => {
      if (request.source.includes('_source.setdefault(') && !request.source.includes('_source.setdefault(0,')) {
        attempts++
        throw Object.assign(new Error('injected failure'), { code })
      }
      return p.execute(request)
    }
    await assert.rejects(p.run(), error => {
      const d = error.bootstrap
      assert.equal(error.code, code)
      assert.equal(d.chunkIndex, 1)
      assert.equal(d.lastSuccessfulOperation.phase, 'chunk')
      assert.equal(d.lastSuccessfulOperation.chunkIndex, 0)
      assert.equal(d.offset, d.lastSuccessfulOperation.offset + d.lastSuccessfulOperation.bytes)
      assert.equal(d.commandIndex, 0)
      return true
    })
    assert.equal(attempts, code === 'TIMEOUT' ? 3 : 1)
  }
})

pythonTest('lost final installation ACK is bounded and never executes the fixture twice', async t => {
  for (const persistent of [false, true]) {
    const p = await peer(t)
    let attempts = 0
    p.device.python.execute = async request => {
      const response = await p.execute(request)
      if (request.source.includes("'install',") && (++attempts === 1 || persistent)) throw timeout()
      return response
    }
    if (persistent) await assert.rejects(p.run(), error => error.code === 'TIMEOUT' && error.bootstrap.phase === 'install' && error.bootstrap.attempt === 3)
    else await p.run()
    assert.equal(attempts, persistent ? 3 : 2)
    assert.equal(await p.inspect(`${namespace}['runs']`), 1)
  }
})

pythonTest('one exhausted prerequisite blocks all dependent checks and preserves independent identity and cleanup evidence', async t => {
  for (const strict of [false, true]) for (const cleanupFails of [false, true]) {
    const p = await peer(t)
    let attempts = 0
    p.device.python.execute = async request => {
      if (request.source.includes('_source.setdefault(')) { attempts++; throw timeout() }
      if (cleanupFails && request.source.startsWith('if ')) throw new Error('cleanup failed')
      return p.execute(request)
    }
    const options = parseArgs(['--tcp', 'test', '--checks', 'memory,camera,espdl_inference,identity', '--model', '/missing-model', ...(strict ? ['--require-all'] : [])])
    await assert.rejects(runDeviceChecks(p.device, options, { nonce, report: r => p.reports.push(r) }), { code: 'DEVICE_CHECKS_FAILED' })
    assert.equal(attempts, 3)
    const failed = p.reports.filter(r => r.status === 'FAIL')
    assert.equal(failed[0].check, 'fixture-bootstrap')
    assert.equal(failed[0].error.code, 'TIMEOUT')
    const d = failed[0].diagnostics
    assert.equal(d.phase, 'chunk')
    assert.equal(d.chunkIndex, 0)
    assert.equal(d.offset, 0)
    assert.ok(d.bytes > 0 && d.inputBytes <= 511 && d.elapsedMs >= 0)
    assert.equal(d.lastSuccessfulOperation.phase, 'init')
    assert.ok(p.reports.filter(r => r.status === 'BLOCKED').every(r => r.blockedBy === 'fixture-bootstrap'))
    assert.deepEqual(p.reports.at(-1), { check: 'summary', passed: 1, failed: cleanupFails ? 2 : 1, skipped: 0, blocked: 3 })
    if (!cleanupFails) assert.equal(await p.inspect(`${JSON.stringify(namespace)} in globals()`), false)
    assert.ok(!p.calls.some(r => r.source.includes("['run_check']")))
  }
})

pythonTest('cancellation does not retry and cleanup uses a fresh signal even when cancellation is a TIMEOUT', async t => {
  for (const cleanupFails of [false, true]) {
    const p = await peer(t), controller = new AbortController(), reason = timeout()
    let attempts = 0, cleaned = false
    p.device.python.execute = async request => {
      if (request.source.includes('_source.setdefault(')) {
        attempts++
        await p.execute(request)
        controller.abort(reason)
        throw reason
      }
      if (request.source.startsWith('if ')) {
        cleaned = true
        assert.equal(request.signal.aborted, false)
        if (cleanupFails) throw new Error('cleanup failed')
      }
      return p.execute(request)
    }
    await assert.rejects(runDeviceChecks(p.device, parseArgs(['--tcp', 'test', '--checks', 'memory,camera']), { nonce, signal: controller.signal, report: r => p.reports.push(r) }), error => error === reason)
    assert.equal(attempts, 1)
    assert.equal(cleaned, true)
    assert.equal(p.reports.some(r => r.event === 'retry'), false)
    assert.equal(p.reports.some(r => r.event === 'cancelled'), true)
    assert.equal(p.reports.some(r => r.check === 'fixture-cleanup' && r.status === 'FAIL'), cleanupFails)
  }
})

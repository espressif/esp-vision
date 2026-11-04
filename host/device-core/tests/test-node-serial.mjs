// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises'
import { delimiter, dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { test } from 'node:test'
import { createDevice } from '../src/device.js'
import { serialTransport } from '../src/node_transport.js'

const coreRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const adapter = join(coreRoot, 'src/node_serial.py')
const tempBase = resolve(process.env.ESP_VISION_NPM_TEMP_DIR ?? process.env.TMPDIR ?? join(coreRoot, '.cache/node-serial'))
const python = [process.env.PYTHON, 'python3', 'python'].filter(Boolean).find(candidate => {
  const result = spawnSync(candidate, ['-c', 'pass'], { encoding: 'utf8', windowsHide: true })
  return result.status === 0
})

async function createSerialFixture() {
  await mkdir(tempBase, { recursive: true })
  const root = await mkdtemp(join(tempBase, 'serial-lock-'))
  const moduleDir = join(root, 'python')
  await mkdir(moduleDir, { recursive: true })
  const stateFile = join(root, 'open-state.json')
  const serialModule = `
import json

class SerialException(Exception):
    pass

class Serial:
    def __init__(self, *args, **kwargs):
        self.rts = None
        self.dtr = None
        self.port = None
        self.exclusive = None

    def open(self):
        with open(${JSON.stringify(stateFile)}, 'w', encoding='utf-8') as stream:
            json.dump({'port': self.port, 'rts': self.rts, 'dtr': self.dtr, 'exclusive': self.exclusive}, stream)
        raise SerialException('Could not exclusively lock port /dev/fixture: [Errno 11] Resource temporarily unavailable')

    def close(self):
        pass
`
  await writeFile(join(moduleDir, 'serial.py'), serialModule)
  return { root, moduleDir, stateFile }
}

function fixturePythonEnv(moduleDir) {
  return {
    ...process.env,
    PYTHONPATH: [moduleDir, process.env.PYTHONPATH].filter(Boolean).join(delimiter),
  }
}

test('POSIX serial open requests an exclusive lock and exposes SERIAL_BUSY', {
  skip: !python || process.platform === 'win32',
}, async (t) => {
  const fixture = await createSerialFixture()
  t.after(() => rm(fixture.root, { recursive: true, force: true }))
  const previousPythonPath = process.env.PYTHONPATH
  process.env.PYTHONPATH = [fixture.moduleDir, previousPythonPath].filter(Boolean).join(delimiter)
  t.after(() => {
    if (previousPythonPath === undefined) delete process.env.PYTHONPATH
    else process.env.PYTHONPATH = previousPythonPath
  })

  const direct = spawnSync(python, ['-u', adapter, '/dev/fixture'], {
    env: fixturePythonEnv(fixture.moduleDir),
    encoding: 'utf8',
    windowsHide: true,
  })
  assert.equal(direct.status, 2, direct.stderr || direct.error?.message || 'Python adapter fixture did not fail as expected')
  assert.equal(direct.stderr.trim(), 'SERIAL_BUSY')
  const opened = JSON.parse(await readFile(fixture.stateFile, 'utf8'))
  assert.deepEqual(opened, { port: '/dev/fixture', rts: false, dtr: true, exclusive: true })
  await rm(fixture.stateFile)

  const device = createDevice({ transport: serialTransport({ python, serial: '/dev/fixture' }) })
  t.after(() => device.disconnect())
  await assert.rejects(device.connect({ timeout: 5000 }), { code: 'SERIAL_BUSY' })
  assert.deepEqual(JSON.parse(await readFile(fixture.stateFile, 'utf8')), opened)
})

test('non-POSIX serial backends do not receive the unsupported exclusive option', {
  skip: !python,
}, async () => {
  const source = [
    'import importlib.util, sys, types',
    'sys.modules["serial"] = types.SimpleNamespace()',
    'spec = importlib.util.spec_from_file_location("node_serial", sys.argv[1])',
    'module = importlib.util.module_from_spec(spec)',
    'spec.loader.exec_module(module)',
    'port = types.SimpleNamespace()',
    'module._configure_serial_exclusive(port, "nt")',
    'assert not hasattr(port, "exclusive")',
  ].join('\n')
  const result = spawnSync(python, ['-c', source, adapter], {
    encoding: 'utf8',
    windowsHide: true,
  })
  assert.equal(result.status, 0, result.stderr || result.error?.message || 'Python fixture failed')
})

test('Node live retune drains without reopening and binary payload cannot become a bridge command', { skip: !python }, async t => {
  await mkdir(tempBase, { recursive: true })
  const root = await mkdtemp(join(tempBase, 'serial-retune-'))
  t.after(() => rm(root, { recursive: true, force: true }))
  const stateFile = join(root, 'state.jsonl')
  await writeFile(join(root, 'serial.py'), `
import json, queue

class SerialException(Exception):
    pass

def record(event, **fields):
    with open(${JSON.stringify(stateFile)}, 'a', encoding='utf-8') as stream:
        stream.write(json.dumps(dict(event=event, **fields)) + '\\n')

class Serial:
    def __init__(self, *args, **kwargs):
        self.queue = queue.Queue()
        self._baudrate = kwargs['baudrate']
    @property
    def baudrate(self):
        return self._baudrate
    @baudrate.setter
    def baudrate(self, value):
        record('baud', value=value)
        if value == 1500000:
            raise ValueError('fixture unsupported baud')
        self._baudrate = value
    def open(self):
        record('open', baud=self.baudrate, rts=self.rts, dtr=self.dtr)
    def read(self, size):
        try:
            return self.queue.get(timeout=0.01)
        except queue.Empty:
            return b''
    def write(self, data):
        record('write', data=data.hex())
        self.queue.put(data)
        return len(data)
    def flush(self):
        record('drain')
    def close(self):
        record('close')
`)
  const previous = process.env.PYTHONPATH
  process.env.PYTHONPATH = [root, previous].filter(Boolean).join(delimiter)
  t.after(() => { if (previous === undefined) delete process.env.PYTHONPATH; else process.env.PYTHONPATH = previous })
  const transport = serialTransport({ python, serial: '/dev/fixture' }), received = []
  let finish
  const echoed = new Promise(resolveEcho => { finish = resolveEcho })
  const data = new Uint8Array([0, 1, 255, ...new TextEncoder().encode('\n{"id":99,"baudRate":1}\n'), 0])
  transport.subscribe({ data(bytes) { received.push(...bytes); if (received.length === data.length) finish() } })
  t.after(() => transport.disconnect())
  await transport.connect({ signal: new AbortController().signal })
  await transport.setBaudRate(2000000)
  await transport.writeBytes(data)
  await Promise.race([echoed, new Promise((_, reject) => { const timer = setTimeout(() => reject(new Error('Binary echo timed out')), 2000); timer.unref() })])
  assert.deepEqual(new Uint8Array(received), data)
  await assert.rejects(transport.setBaudRate(1500000), { code: 'SERIAL_BAUD_UNSUPPORTED' })
  await transport.setBaudRate(115200)
  assert.equal(transport.getDiagnostics().baudRate, 115200)
  const state = (await readFile(stateFile, 'utf8')).trim().split('\n').map(line => JSON.parse(line))
  assert.equal(state.filter(event => event.event === 'open').length, 1)
  assert.deepEqual(state.filter(event => event.event === 'baud').map(event => event.value), [2000000, 1500000, 115200])
  assert.equal(state.filter(event => event.event === 'drain').length, 3)
  assert.deepEqual(state.find(event => event.event === 'open'), { event: 'open', baud: 115200, rts: false, dtr: true })
})

async function drainFixture(t, flushPlan = ['gate']) {
  await mkdir(tempBase, { recursive: true })
  const root = await mkdtemp(join(tempBase, 'serial-drain-'))
  t.after(() => rm(root, { recursive: true, force: true }))
  const stateFile = join(root, 'state.jsonl'), gateFile = join(root, 'flush-gate')
  await writeFile(join(root, 'serial.py'), `
import json, os, sys, time

PLAN = json.loads(${JSON.stringify(JSON.stringify(flushPlan))})

class SerialException(Exception):
    pass

def record(event, **fields):
    with open(${JSON.stringify(stateFile)}, 'a', encoding='utf-8') as stream:
        stream.write(json.dumps(dict(event=event, **fields)) + '\\n')

class Serial:
    def __init__(self, *args, **kwargs):
        self.baudrate = kwargs['baudrate']
        self.flush_count = 0
    def open(self):
        record('open', baud=self.baudrate)
    def read(self, size):
        time.sleep(0.01)
        return b''
    def write(self, data):
        # Exercise the production bridge's partial serial-write loop.
        written = min(3, len(data))
        record('write', data=data[:written].hex())
        return written
    def flush(self):
        self.flush_count += 1
        count = self.flush_count
        mode = PLAN[min(count - 1, len(PLAN) - 1)]
        record('drain_start', count=count, baud=self.baudrate)
        if mode == 'error':
            raise SerialException('fixture drain failure: private port details')
        if mode == 'exit':
            raise SystemExit(2)
        if mode == 'wrong_ack':
            print(json.dumps({'id': 1, 'ok': True, 'baudRate': self.baudrate}), file=sys.stderr, flush=True)
        elif mode == 'gate':
            while not os.path.exists(${JSON.stringify(gateFile)}):
                time.sleep(0.005)
        else:
            time.sleep(mode)
        record('drain_end', count=count, baud=self.baudrate)
    def close(self):
        record('close')
`)
  const previous = process.env.PYTHONPATH
  process.env.PYTHONPATH = [root, previous].filter(Boolean).join(delimiter)
  t.after(() => { if (previous === undefined) delete process.env.PYTHONPATH; else process.env.PYTHONPATH = previous })
  const transport = serialTransport({ python, serial: '/dev/fixture' })
  t.after(() => transport.disconnect())
  await transport.connect({ signal: new AbortController().signal })
  const state = async () => (await readFile(stateFile, 'utf8')).trim().split('\n').map(line => JSON.parse(line))
  const waitFor = async (event, count = 1) => {
    const deadline = Date.now() + 3000
    while (Date.now() < deadline) {
      const records = await state()
      if (records.filter(record => record.event === event).length >= count) return records
      await new Promise(resolveWait => setTimeout(resolveWait, 5))
    }
    throw new Error(`Serial fixture did not reach ${event}`)
  }
  return { transport, state, waitFor, unblock: () => writeFile(gateFile, '') }
}

test('Node drain waits for real Python port.flush after all partial byte writes without changing baud', { skip: !python }, async t => {
  const f = await drainFixture(t)
  const bytes = new TextEncoder().encode('import machine; machine.bootloader()\r\n')
  await f.transport.writeBytes(bytes)
  let completed = false
  const draining = f.transport.drain().then(() => { completed = true })
  const before = await f.waitFor('drain_start')
  assert.equal(completed, false, 'accepting the child stdin record is not a drain ACK')
  assert.deepEqual(Buffer.from(before.filter(record => record.event === 'write').map(record => record.data).join(''), 'hex'), Buffer.from(bytes))
  assert.equal(before.some(record => record.event === 'drain_end'), false)
  await f.unblock()
  await draining
  assert.equal(f.transport.getDiagnostics().baudRate, 115200)
  await f.transport.disconnect()
  const after = await f.state()
  assert.deepEqual(after.filter(record => ['open', 'drain_start', 'drain_end', 'close'].includes(record.event)).map(record => record.event), ['open', 'drain_start', 'drain_end', 'close'])
  assert.deepEqual(after.filter(record => record.event.startsWith('drain')).map(record => record.baud), [115200, 115200])
})

test('Node drain distinguishes flush rejection and a baud-shaped ACK from successful drain', { skip: !python }, async t => {
  const f = await drainFixture(t, ['error', 0])
  await assert.rejects(f.transport.drain(), { code: 'SERIAL_DRAIN_FAILED', message: 'Serial adapter could not drain pending output' })
  await f.transport.drain()
  assert.equal(f.transport.getDiagnostics().baudRate, 115200)
  await f.transport.disconnect()
  const wrong = await drainFixture(t, ['wrong_ack'])
  await assert.rejects(wrong.transport.drain(), { code: 'SERIAL_DRAIN_FAILED' })
  assert.equal(wrong.transport.getDiagnostics().baudRate, 115200)
})

test('Node drain preserves pre-cancellation and ignores a cancelled request late ACK', { skip: !python }, async t => {
  const f = await drainFixture(t, ['gate', 0.05])
  const before = new AbortController(), reason = Object.assign(new Error('cancel drain'), { code: 'CANCELLED' })
  before.abort(reason)
  await assert.rejects(f.transport.drain({ signal: before.signal }), error => error === reason)
  assert.equal((await f.state()).some(record => record.event === 'drain_start'), false)
  const controller = new AbortController()
  const pending = f.transport.drain({ signal: controller.signal })
  const rejected = assert.rejects(pending, error => error === reason)
  await f.waitFor('drain_start')
  controller.abort(reason)
  await rejected
  const next = f.transport.drain()
  await f.unblock()
  await next
  assert.equal((await f.state()).filter(record => record.event === 'drain_end').length, 2, 'the previous ID must not resolve the next request')
})

test('Node drain timeout remains distinct and its late ACK cannot resolve the next drain', { skip: !python }, async t => {
  const f = await drainFixture(t, ['gate', 0.05])
  await assert.rejects(f.transport.drain(), { code: 'SERIAL_DRAIN_TIMEOUT', message: 'Serial drain timed out' })
  const next = f.transport.drain()
  await f.unblock()
  await next
  assert.equal((await f.state()).filter(record => record.event === 'drain_end').length, 2)
})

test('Node drain rejects promptly on explicit disconnect or Python bridge exit', { skip: !python }, async t => {
  const f = await drainFixture(t)
  const pending = f.transport.drain()
  const rejected = assert.rejects(pending, { code: 'SERIAL_CLOSED' })
  await f.waitFor('drain_start')
  await f.transport.disconnect()
  await rejected
  const exited = await drainFixture(t, ['exit'])
  await assert.rejects(exited.transport.drain(), { code: 'SERIAL_CLOSED' })
})

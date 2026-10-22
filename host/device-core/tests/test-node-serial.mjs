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

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises'
import { delimiter, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { explainSerialFailure } from '../src/serial_diagnostics.js'
import { parseCli, runCli, usage } from '../src/cli.js'

const fail = (code = 'SERIAL_ERROR') => Object.assign(new Error('original serial failure'), { code })
const options = { python: 'C:\\Python Path\\selected-python.exe', timeout: 5000 }

test('real interpreter runs the fixed import check and never leaks module errors', async t => {
  const python = [process.env.PYTHON, 'python3', 'python', 'py'].filter(Boolean).find(command => spawnSync(command, ['-c', 'pass']).status === 0)
  if (!python) return t.skip('Python is unavailable')
  const base = process.env.TMPDIR
  assert.ok(base, 'Use a project-local TMPDIR for subprocess fixtures')
  await mkdir(base, { recursive: true })
  const directory = await mkdtemp(join(base, 'cli-import-'))
  t.after(() => rm(directory, { recursive: true, force: true }))
  const previous = process.env.PYTHONPATH
  process.env.PYTHONPATH = [directory, previous].filter(Boolean).join(delimiter)
  t.after(() => { if (previous === undefined) delete process.env.PYTHONPATH; else process.env.PYTHONPATH = previous })
  for (const [module, expected] of [
    ["raise ModuleNotFoundError('PRIVATE_MISSING', name='serial')", /has no pyserial module/],
    ["raise ModuleNotFoundError('PRIVATE_NESTED', name='nested_dependency')", /could not import pyserial successfully/],
    ["raise RuntimeError('PRIVATE_IMPORT')", /could not import pyserial successfully/],
    ['OTHER = True', /conflicting serial.py/],
    ['class Serial: pass', /Serial API are available/],
  ]) {
    await writeFile(join(directory, 'serial.py'), module + '\n')
    await rm(join(directory, '__pycache__'), { recursive: true, force: true })
    const failure = await explainSerialFailure(fail(), { python, timeout: 3000 })
    assert.match(failure.message, expected)
    assert.doesNotMatch(failure.message, /PRIVATE_/)
  }
  const missing = await explainSerialFailure(fail(), { python: join(directory, 'nonexistent-python'), timeout: 3000 })
  assert.match(missing.message, /executable was not found/)
  await writeFile(join(directory, 'serial.py'), 'import time; time.sleep(10)\n')
  const started = Date.now()
  const timedOut = await explainSerialFailure(fail(), { python, timeout: 50 })
  assert.match(timedOut.message, /could not be established/)
  assert.ok(Date.now() - started < 3000, 'Import diagnosis must obey its time budget')
})

test('failed serial setup distinguishes interpreter, module, shadowing and unknown failures without copying output', async () => {
  for (const [code, message] of [
    ['ENOENT', /executable was not found.*--python py/],
    ['EACCES', /executable.*access was denied/],
    [71, /same interpreter.*-m pip install pyserial/],
    [72, /conflicting serial.py/],
    [73, /could not import pyserial successfully/],
    ['ETIMEDOUT', /could not be established/],
    [1, /could not be established/],
  ]) {
    let calls = 0
    const failure = await explainSerialFailure(fail(), options, { async execFile(command, args, settings) {
      calls++
      assert.equal(command, options.python)
      assert.equal(args[0], '-c')
      assert.equal(args.length, 2)
      assert.equal(settings.timeout, 3000)
      assert.equal(settings.maxBuffer, 4096)
      assert.equal(settings.shell, undefined)
      assert.equal(settings.windowsHide, true)
      throw Object.assign(new Error('PRIVATE_EXCEPTION'), { code, stdout: 'PRIVATE_STDOUT', stderr: 'PRIVATE_STDERR' })
    } })
    assert.equal(calls, 1)
    assert.equal(failure.code, 'SERIAL_ERROR')
    assert.match(failure.message, message)
    assert.doesNotMatch(failure.message, /PRIVATE_/)
  }
})

test('healthy Python does not turn a serial failure into a missing-dependency claim', async () => {
  const failure = await explainSerialFailure(fail(), options, { async execFile() { return { stdout: 'ignored' } } })
  assert.match(failure.message, /Python and the pyserial Serial API are available/)
  assert.match(failure.message, /another application/)
  assert.doesNotMatch(failure.message, /pip install/)
})

test('busy, denied, runtime I/O and unrelated failures never start an environment diagnosis', async () => {
  const dependencies = { execFile() { throw new Error('Unexpected process') } }
  for (const code of ['SERIAL_BUSY', 'SERIAL_ACCESS_DENIED']) {
    const failure = await explainSerialFailure(fail(code), options, dependencies)
    assert.equal(failure.code, code)
    assert.match(failure.message, /other|another|applications/)
    assert.doesNotMatch(failure.message, /pip install/)
  }
  for (const stage of ['read', 'write', 'stdin', 'stdout']) {
    const failure = Object.assign(fail(), { details: { stage } })
    assert.equal((await explainSerialFailure(failure, options, dependencies)).message, 'original serial failure')
  }
  assert.equal((await explainSerialFailure(fail('TCP_ERROR'), options, dependencies)).message, 'original serial failure')
})

test('cancellation and bounded timeout preserve the original failure and the same abort signal', async () => {
  const controller = new AbortController()
  const failure = fail()
  await explainSerialFailure(failure, { ...options, timeout: 5 }, { signal: controller.signal, async execFile(_command, _args, settings) {
    assert.equal(settings.timeout, 5)
    assert.equal(settings.signal, controller.signal)
    controller.abort()
    throw new Error('aborted')
  } })
  assert.equal(failure.message, 'original serial failure')
  await explainSerialFailure(failure, options, { signal: controller.signal, execFile() { throw new Error('Unexpected process') } })
  assert.equal(failure.message, 'original serial failure')
})

test('serial failure cleanup finishes before diagnosis; TCP failures skip Python entirely', async () => {
  for (const endpoint of [['--port', 'fixture'], ['--tcp', 'fixture']]) {
    const calls = []
    await assert.rejects(runCli(parseCli(['inspect', ...endpoint]), {
      transportFor: value => value,
      createDevice() { return { async connect() { throw fail() }, async disconnect() { calls.push('disconnect') } } },
      async execFile() { calls.push('diagnose'); throw Object.assign(new Error('missing'), { code: 'ENOENT' }) },
    }), error => {
      assert.equal(error.code, 'SERIAL_ERROR')
      assert.match(error.message, endpoint[0] === '--port' ? /--python py/ : /original serial failure/)
      return true
    })
    assert.deepEqual(calls, endpoint[0] === '--port' ? ['disconnect', 'diagnose'] : ['disconnect'])
  }
})

test('successful serial operations do not spawn an extra Python process', async () => {
  const result = await runCli(parseCli(['inspect', '--port', 'fixture']), {
    transportFor: endpoint => endpoint,
    createDevice() { return { async connect() {}, async inspect() { return { identity: 'fixture' } }, async disconnect() {} } },
    execFile() { throw new Error('Successful connection triggered diagnosis') },
  })
  assert.deepEqual(result, { identity: 'fixture' })
})

test('serial inventory diagnostics preserve successful TCP discovery and useful serial-only failure', async () => {
  const dependencies = {
    async listSerial() { throw fail('SERIAL_UNAVAILABLE') },
    async listNetwork() { return [{ addresses: ['192.0.2.1'], port: 3232 }] },
    async execFile() { throw Object.assign(new Error('missing'), { code: 'ENOENT' }) },
    createDevice() { throw new Error('Inventory must not open a device') },
  }
  const listing = await runCli(parseCli(['inspect']), dependencies)
  assert.equal(listing.sourcesAvailable, 1)
  assert.equal(listing.devices[0].interface, 'tcp')
  assert.match(listing.warnings[0].message, /--python py/)
  await assert.rejects(runCli(parseCli(['inspect', '--interface', 'serial']), dependencies), error => {
    assert.equal(error.code, 'ENUMERATION_UNAVAILABLE')
    assert.match(error.message, /--python py/)
    return true
  })
})

test('help and offline firmware checks never diagnose serial setup', async () => {
  const dependencies = {
    execFile() { throw new Error('Unexpected process') },
    createDevice() { return { firmware: { async check() { return { release: null } } }, async disconnect() {} } },
  }
  await runCli(parseCli(['--help']), dependencies)
  await runCli(parseCli(['firmware', 'check', '--board', 'FIXTURE']), dependencies)
})

test('real help describes camera versus LCD without claiming a fresh or always overlay-free frame', () => {
  const entry = fileURLToPath(new URL('../src/cli.js', import.meta.url))
  for (const json of [false, true]) {
    const result = spawnSync(process.execPath, [entry, '--help', ...(json ? ['--json'] : [])], { encoding: 'utf8' })
    assert.equal(result.status, 0, result.stderr)
    const text = json ? JSON.parse(result.stdout).result.usage : result.stdout.trimEnd()
    assert.equal(text, usage)
    assert.match(text, /camera:.*not the LCD image/)
    assert.match(text, /screen:.*LCD framebuffer.*drawing/)
    assert.match(text, /--python py/)
    assert.doesNotMatch(text, /raw sensor|no (display )?overlays/)
  }
})

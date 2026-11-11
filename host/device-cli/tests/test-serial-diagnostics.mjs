// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { explainSerialFailure } from '../src/serial_diagnostics.js'
import { parseCli, runCli, usage } from '../src/cli.js'

const fail = (code = 'SERIAL_ERROR') => Object.assign(new Error('original serial failure'), { code })
const options = { python: 'C:\\Python Path\\selected-python.exe', timeout: 5000 }

test('missing npm native backend has actionable guidance without invoking an interpreter', async () => {
  const failure = await explainSerialFailure(fail('SERIAL_BACKEND_UNAVAILABLE'), options, { execFile() { assert.fail('No process') } })
  assert.match(failure.message, /npm serial backend/)
  assert.doesNotMatch(failure.message, /pip|python/i)
  assert.equal((await explainSerialFailure(fail(), options)).message, 'original serial failure')
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
      assert.match(error.message, /original serial failure/)
      return true
    })
    assert.deepEqual(calls, ['disconnect'])
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
  assert.match(listing.warnings[0].message, /original serial failure/)
  await assert.rejects(runCli(parseCli(['inspect', '--interface', 'serial']), dependencies), error => {
    assert.equal(error.code, 'ENUMERATION_UNAVAILABLE')
    assert.match(error.message, /original serial failure/)
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
    assert.match(text, /no host Python/)
    assert.doesNotMatch(text, /raw sensor|no (display )?overlays/)
  }
})

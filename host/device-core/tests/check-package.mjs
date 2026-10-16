// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { execFileSync, spawnSync } from 'node:child_process'
import { mkdtemp, readFile, rm, mkdir } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import process from 'node:process'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const manifest = JSON.parse(await readFile(join(root, 'package.json'), 'utf8'))
assert.equal(manifest.license, 'Apache-2.0')
assert.match(await readFile(join(root, 'LICENSE'), 'utf8'), /Apache License[\s\S]*Version 2\.0/)
const temporary = await mkdtemp(join(tmpdir(), 'esp-vision-device-core-package-'))
try {
  const [packed] = JSON.parse(execFileSync('npm', ['pack', '--json', '--ignore-scripts', '--pack-destination', temporary], { cwd: root, encoding: 'utf8' }))
  assert.ok(packed.files.some(({ path }) => path === 'LICENSE'))
  assert.ok(packed.files.some(({ path }) => path === 'NOTICE'))
  assert.ok(packed.files.some(({ path }) => path === 'schemas/device-actions-v1.json'))
  assert.ok(packed.files.every(({ path }) => /^(src\/|schemas\/|tests\/|scripts\/|package\.json$|README\.md$|LICENSE$|NOTICE$|CHANGELOG\.md$)/.test(path)), 'tarball must contain only the standalone library')
  assert.ok(!packed.files.some(({ path }) => path.startsWith('docs/')), 'Documentation is published by the ESP-VISION documentation center')
  assert.ok(!packed.files.some(({ path }) => path.includes('__pycache__') || path.endsWith('.pyc')), 'Never ship host Python bytecode')
  const consumer = join(temporary, 'consumer')
  await mkdir(consumer)
  execFileSync('npm', ['install', '--offline', '--ignore-scripts', '--no-audit', '--no-fund', '--package-lock=false', join(temporary, packed.filename)], { cwd: consumer, stdio: 'pipe' })
  const installed = join(consumer, 'node_modules/@esp-vision/device-core')
  assert.equal(await readFile(join(installed, 'LICENSE'), 'utf8'), await readFile(join(root, 'LICENSE'), 'utf8'))
  const notice = await readFile(join(installed, 'NOTICE'), 'utf8')
  assert.ok(notice.includes('Copyright (c) 2024 Volodymyr Shymanskyy'))
  assert.ok(notice.includes('Permission is hereby granted, free of charge'))
  const entryPoints = Object.keys(manifest.exports).map(key => manifest.name + (key === '.' ? '' : key.slice(1)))
  const script = `for (const name of ${JSON.stringify(entryPoints)}) {
    const module = await import(name);
    if (!Object.keys(module).length) throw new Error('Empty entry point: ' + name);
  }
  const api = await import('@esp-vision/device-core');
  if (Object.keys(api).sort().join(',') !== 'createDevice') throw new Error('Root must expose only the application API');
  const { DeviceSession } = await import('@esp-vision/device-core/session');
  const { FirmwareFlashSession } = await import('@esp-vision/device-core/firmware');
  const { tcpTransport, serialTransport, openNodeRom } = await import('@esp-vision/device-core/node');
  if (typeof tcpTransport !== 'function' || typeof serialTransport !== 'function') throw new Error('Missing public Node transports');
  const { createNodeDiscovery } = await import('@esp-vision/device-core/discovery/node');
  const { EventEmitter } = await import('node:events');
  let queries = 0;
  const provider = createNodeDiscovery({ createMdns() {
    const mdns = new EventEmitter(); mdns.query = () => { queries++ }; mdns.destroy = () => {}; return mdns;
  } });
  const device = api.createDevice({ discovery: provider });
  if (typeof provider.list !== 'function') throw new Error('Discovery providers must expose list()');
  if ((await device.discover({ timeout: 50 })).length !== 0 || !queries) throw new Error('Device discovery did not call its provider');
  if (typeof openNodeRom !== 'function' || typeof openNodeRom({ port: 'not-opened' }).initialize !== 'function') throw new Error('Missing passive Node ROM adapter');
  // Constructors must be passive: these endpoints need not exist to create adapters.
  api.createDevice({ transport: tcpTransport({ host: '127.0.0.1', port: 1 }) });
  api.createDevice({ transport: serialTransport({ python: 'python3', serial: 'not-opened' }) });
  if (typeof DeviceSession !== 'function' || typeof FirmwareFlashSession !== 'function') throw new Error('Missing host adapter APIs');`
  execFileSync(process.execPath, ['--input-type=module', '-e', script], { cwd: consumer, stdio: 'pipe' })
  assert.equal(manifest.bin['esp-vision-device'], './src/cli.js')
  const help = execFileSync(process.execPath, [join(consumer, 'node_modules/@esp-vision/device-core/src/cli.js'), '--help'], { encoding: 'utf8' })
  assert.ok(help.includes('esp-vision-device inspect'))
  if (process.platform !== 'win32') {
    const installedHelp = execFileSync(join(consumer, 'node_modules/.bin/esp-vision-device'), ['--help'], { encoding: 'utf8' })
    assert.ok(installedHelp.includes('esp-vision-device inspect'), 'The installed npm command must execute through its symlink')
  }
  for (const required of ['src/node_serial.py', 'scripts/run-tests.mjs', 'tests/hardware-check.mjs', 'tests/device-checks.mjs', 'tests/fixtures/device-checks.py']) {
    assert.ok(packed.files.some(({ path }) => path === required), `Missing installed check asset: ${required}`)
  }
  const unitOutput = execFileSync('npm', ['test'], { cwd: installed, encoding: 'utf8', maxBuffer: 4 * 1024 * 1024 })
  const unitCount = Number(/^# tests (\d+)$/m.exec(unitOutput)?.[1] ?? 0)
  assert.ok(unitCount > 0, 'Installed npm test must execute a nonempty suite')
  const hardwareHelp = execFileSync('npm', ['run', 'test:hardware', '--', '--help'], { cwd: installed, encoding: 'utf8' })
  assert.ok(hardwareHelp.includes('--serial PATH'), 'Installed hardware runner must load and show its help')
  // A damaged tarball must fail instead of reporting a successful zero-test run.
  await rm(join(installed, 'tests'), { recursive: true })
  await mkdir(join(installed, 'tests'))
  const empty = spawnSync('npm', ['test'], { cwd: installed, encoding: 'utf8' })
  assert.equal(empty.status, 1, 'An empty suite must fail closed')
  assert.match(empty.stderr, /No Device Core unit tests found/)
  await rm(join(installed, 'tests'), { recursive: true })
  const missing = spawnSync('npm', ['test'], { cwd: installed, encoding: 'utf8' })
  assert.equal(missing.status, 1, 'Missing tests must fail closed')
  console.log(JSON.stringify({ package: packed.name, version: packed.version, entryPoints, unitCount, standalone: true }))
} finally {
  await rm(temporary, { recursive: true, force: true })
}

// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { mkdtemp, readFile, rm, mkdir } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import process from 'node:process'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const manifest = JSON.parse(await readFile(join(root, 'package.json'), 'utf8'))
assert.equal(manifest.license, 'Apache-2.0')
assert.equal(await readFile(join(root, 'LICENSE'), 'utf8'), await readFile(join(root, '../../LICENSE'), 'utf8'))
const temporary = await mkdtemp(join(tmpdir(), 'esp-vision-device-core-package-'))
try {
  const [packed] = JSON.parse(execFileSync('npm', ['pack', '--json', '--ignore-scripts', '--pack-destination', temporary], { cwd: root, encoding: 'utf8' }))
  assert.ok(packed.files.some(({ path }) => path === 'LICENSE'))
  assert.ok(packed.files.some(({ path }) => path === 'NOTICE'))
  assert.ok(packed.files.some(({ path }) => path === 'schemas/device-actions-v1.json'))
  assert.ok(packed.files.every(({ path }) => /^(src\/|schemas\/|package\.json$|README\.md$|LICENSE$|NOTICE$|CHANGELOG\.md$)/.test(path)), 'tarball must contain only the standalone library')
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
  if (typeof DeviceSession !== 'function' || typeof FirmwareFlashSession !== 'function') throw new Error('Missing host adapter APIs');`
  execFileSync(process.execPath, ['--input-type=module', '-e', script], { cwd: consumer, stdio: 'pipe' })
  assert.equal(manifest.bin['esp-vision-device'], './src/cli.js')
  const help = execFileSync(process.execPath, [join(consumer, 'node_modules/@esp-vision/device-core/src/cli.js'), '--help'], { encoding: 'utf8' })
  assert.ok(help.includes('esp-vision-device inspect'))
  if (process.platform !== 'win32') {
    const installedHelp = execFileSync(join(consumer, 'node_modules/.bin/esp-vision-device'), ['--help'], { encoding: 'utf8' })
    assert.ok(installedHelp.includes('esp-vision-device inspect'), 'The installed npm command must execute through its symlink')
  }
  assert.ok(packed.files.some(({ path }) => path === 'src/node_serial.py'))
  console.log(JSON.stringify({ package: packed.name, version: packed.version, entryPoints, standalone: true }))
} finally {
  await rm(temporary, { recursive: true, force: true })
}

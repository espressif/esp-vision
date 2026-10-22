// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { chmod, mkdir, mkdtemp, readFile, writeFile } from 'node:fs/promises'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const artifactRoot = resolve(process.env.TMPDIR || join(root, '../../.cache/device-cli-check'))
await mkdir(artifactRoot, { recursive: true })
const args = process.argv.slice(2)
if (args.length !== 2 || args[0] !== '--tarball') throw new Error('Usage: node scripts/check-package.mjs --tarball FILE')
const tarball = resolve(args[1])
const workspace = await mkdtemp(join(artifactRoot, 'device-cli-package-check-'))
const install = join(workspace, 'installed')
const python = join(workspace, 'python-list')
await mkdir(install, { recursive: true })

function run(command, commandArgs, cwd) {
  const result = spawnSync(command, commandArgs, {
    cwd,
    encoding: 'utf8',
    env: {
      ...process.env,
      TMPDIR: artifactRoot,
      TMP: artifactRoot,
      TEMP: artifactRoot,
      npm_config_cache: join(artifactRoot, 'npm-cache'),
    },
    maxBuffer: 8 * 1024 * 1024,
  })
  if (result.error) throw result.error
  if (result.status !== 0) throw new Error(`${command} ${commandArgs.join(' ')} failed (${result.status}):\n${result.stderr || result.stdout}`)
  return result.stdout
}

const tarballHeader = await readFile(tarball)
assert.equal(tarballHeader.subarray(0, 2).toString('hex'), '1f8b', 'Expected an npm gzip tarball')
await writeFile(python, '#!/bin/sh\ncase "$1" in */node_serial.py) test -f "$1" ;; *) exit 91 ;; esac\nprintf "[]\\n"\n')
await chmod(python, 0o755)
run('npm', ['install', '--prefix', install, '--no-save', '--no-package-lock', '--ignore-scripts', '--no-audit', '--no-fund', tarball], root)
const bin = join(install, 'node_modules/.bin/esp-vision-device')
const helpOutput = run(bin, ['--help', '--json'], install)
assert.equal(helpOutput.trim().split(/\r?\n/).length, 1, 'Machine help must be one JSON line on stdout')
const helpEnvelope = JSON.parse(helpOutput)
assert.equal(helpEnvelope.schemaVersion, 1)
assert.equal(helpEnvelope.ok, true)
assert.equal(helpEnvelope.command, 'help')
assert.ok(helpEnvelope.result.capabilities.commands.python.includes('--source CODE'))
assert.ok(helpEnvelope.result.capabilities.commands.watch)
assert.equal(helpEnvelope.result.capabilities.output.events, 'NDJSON on stderr')
const output = run(bin, ['inspect', '--interface', 'serial', '--python', python, '--json'], install)
const envelope = JSON.parse(output)
assert.deepEqual(Object.keys(envelope), ['schemaVersion', 'ok', 'command', 'result'])
assert.equal(envelope.schemaVersion, 1)
assert.equal(envelope.ok, true)
assert.equal(envelope.command, 'inspect')
assert.deepEqual(envelope.result.devices, [])
assert.equal(envelope.result.sourcesAvailable, 1)
for (const [command, args] of [
  ['python', ['python', '--path', '/app.py', '--completion', 'start', '--timeout', '100']],
  ['control', ['control', '--action', 'stop', '--mode', 'soft']],
]) {
  const rejected = spawnSync(bin, [...args, '--port', '/nonexistent', '--json'], {
    cwd: install, encoding: 'utf8', env: process.env,
  })
  assert.equal(rejected.status, 2, rejected.stderr)
  const failure = JSON.parse(rejected.stdout)
  assert.equal(failure.ok, false)
  assert.equal(failure.command, command, 'Installed CLI must retain rejected command identity')
  assert.equal(failure.error.code, 'ARGUMENT')
  assert.ok(!rejected.stderr.includes('"state":"connecting"'))
}
const packageFiles = run('tar', ['-tzf', tarball], root).split(/\r?\n/)
for (const required of [
  'package/dist/device-cli.mjs', 'package/dist/node_serial.py', 'package/dist/node_rom.py',
  'package/LICENSE', 'package/DEVICE_CORE_NOTICE.md', 'package/THIRD_PARTY_NOTICES.md',
  'package/licenses/smol-toml-LICENSE',
]) assert.ok(packageFiles.includes(required), `Tarball is missing ${required}`)
console.log(`Installed tarball passed: ${tarball}`)

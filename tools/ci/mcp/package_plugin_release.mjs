// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Package the plugin with the published, pinned CLI, not the checkout's CLI.
import assert from 'node:assert/strict'
import { minorRange, inMinorRange, cliBundlesCore } from '../../../host/device-core/scripts/version-policy.mjs'
import { createHash } from 'node:crypto'
import { mkdir, mkdtemp, readFile } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..')
const plugin = join(root, 'host/esp-vision-plugin')
const args = process.argv.slice(2)
assert.equal(args.length, 2)
assert.equal(args[0], '--output')
const output = resolve(args[1])
await mkdir(output, { recursive: true })
const temp = await mkdtemp(join(process.env.TMPDIR ?? output, 'plugin-cli-release-'))
const manifest = JSON.parse(await readFile(join(plugin, 'package.json'), 'utf8'))
const { deviceCore, deviceCli } = manifest.espVision ?? {}
const knowledge = JSON.parse(await readFile(join(root, 'host/knowledge-mcp/package.json'), 'utf8'))
assert.deepEqual(manifest.espVision, knowledge.espVision, 'Plugin and Knowledge must declare matching release pins')
for (const range of [deviceCore, deviceCli]) minorRange(range)
const resolved = manifest.espVision?.resolved
assert.ok(inMinorRange(deviceCore, resolved?.deviceCore), 'Resolved Core must satisfy its minor range')
assert.ok(inMinorRange(deviceCli, resolved?.deviceCli), 'Resolved CLI must satisfy its minor range')
const registry = 'https://registry.npmjs.org/'
async function metadata(name, version) {
  const response = await fetch(new URL(`${encodeURIComponent(name)}/${encodeURIComponent(version)}`, registry), { signal: AbortSignal.timeout(15000), cache: 'no-store' })
  assert.equal(response.status, 200, `Required published package ${name}@${version} is unavailable`)
  const data = await response.json()
  assert.equal(data.name, name)
  assert.equal(data.version, version)
  assert.match(data.dist?.integrity ?? '', /^sha512-/)
  return data
}
function run(command, args, options = {}) {
  const result = spawnSync(command, args, { cwd: options.cwd ?? root, env: { ...process.env, ...options.env }, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 })
  assert.equal(result.status, 0, `${command} failed: ${result.stderr || result.stdout || result.error?.message || ''}`)
  return result.stdout
}
const core = await metadata('@esp-vision/device-core', resolved.deviceCore)
const cli = await metadata('@esp-vision/device-cli', resolved.deviceCli)
assert.ok(cliBundlesCore(cli, resolved.deviceCore), 'Published CLI must bundle the resolved Core version')
const [packed] = JSON.parse(run('npm', ['pack', `@esp-vision/device-cli@${resolved.deviceCli}`, '--json', '--ignore-scripts', '--pack-destination', temp, '--registry', registry]))
const tarball = join(temp, packed.filename)
const integrity = 'sha512-' + createHash('sha512').update(await readFile(tarball)).digest('base64')
assert.equal(integrity, cli.dist.integrity, 'Downloaded CLI differs from npm registry integrity')
const listing = run('tar', ['-tzf', tarball]).trim().split('\n')
const types = run('tar', ['-tvzf', tarball]).trim().split('\n')
assert.ok(types.every(line => /^[-d]/.test(line)), 'CLI archive must contain only files and directories')
assert.ok(listing.every(path => path.startsWith('package/') && !path.split('/').includes('..')), 'Unsafe CLI archive path')
run('tar', ['-xzf', tarball, '-C', temp])
const cliDir = join(temp, 'package')
const extracted = JSON.parse(await readFile(join(cliDir, 'package.json'), 'utf8'))
assert.equal(extracted.name, cli.name)
assert.equal(extracted.version, resolved.deviceCli)
assert.ok(cliBundlesCore(extracted, resolved.deviceCore))
run(process.execPath, ['scripts/package.mjs'], { cwd: plugin, env: { ESP_VISION_PLUGIN_CLI_PACKAGE: cliDir } })
run('npm', ['run', 'check:package'], { cwd: plugin })
run('npm', ['test'], { cwd: plugin })
const [result] = JSON.parse(run('npm', ['pack', '--json', '--ignore-scripts', '--pack-destination', output], { cwd: plugin }))
assert.equal(result.name, manifest.name)
assert.equal(result.version, manifest.version)
console.log(`Prepared ${manifest.name}@${manifest.version} with published CLI ${resolved.deviceCli} (${integrity}) and Core ${resolved.deviceCore} (${core.dist.integrity}).`)

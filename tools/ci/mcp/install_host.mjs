#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// MR integration uses an explicit Core tarball; tracked manifests remain registry-only.
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { cp, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..')
const args = process.argv.slice(2)
assert.ok(args.length === 2 && args[0] === '--output', 'Usage: node tools/ci/mcp/install_host.mjs --output DIRECTORY')
const output = resolve(args[1])
await mkdir(output, { recursive: true })
const temporary = await mkdtemp(join(output, '.install-'))
const env = { ...process.env, TMPDIR: temporary, TMP: temporary, TEMP: temporary }
function npm(args, cwd) {
  const result = spawnSync('npm', args, { cwd, env, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 })
  if (result.status !== 0) throw new Error(`npm ${args[0]} failed: ${result.stderr || result.error?.message || result.status}`)
  return result.stdout
}
try {
  const core = join(root, 'host/device-core')
  npm(['install', '--ignore-scripts', '--no-audit', '--no-fund', '--package-lock=false'], core)
  const [packed] = JSON.parse(npm(['pack', '--json', '--ignore-scripts', '--pack-destination', output], core))
  const tarball = join(output, packed.filename)
  const bytes = await readFile(tarball)
  const receipt = { schemaVersion: 1, name: packed.name, version: packed.version, file: packed.filename,
    sha256: createHash('sha256').update(bytes).digest('hex'), integrity: packed.integrity }
  await writeFile(join(output, 'core-artifact.json'), JSON.stringify(receipt, null, 2) + '\n')
  // Validate an independent consumer and prime metadata needed by Core's
  // subsequent offline installed-package checks, including on a fresh cache.
  const sdkConsumer = join(temporary, 'core-consumer')
  await mkdir(sdkConsumer)
  await writeFile(join(sdkConsumer, 'package.json'), '{"private":true}\n')
  npm(['install', '--ignore-scripts', '--no-audit', '--no-fund', tarball], sdkConsumer)
  for (const name of ['device-cli']) {
    const source = join(root, 'host', name)
    const manifest = JSON.parse(await readFile(join(source, 'package.json'), 'utf8'))
    assert.equal(manifest.devDependencies?.['@esp-vision/device-core'], packed.version,
      `${name} must pin the staged Core version`)
    const consumer = join(temporary, name)
    await mkdir(consumer)
    manifest.devDependencies['@esp-vision/device-core'] = `file:${tarball}`
    await writeFile(join(consumer, 'package.json'), JSON.stringify(manifest, null, 2) + '\n')
    try {
      const lock = JSON.parse(await readFile(join(source, 'package-lock.json'), 'utf8'))
      const coreLock = lock.packages['node_modules/@esp-vision/device-core']
      assert.equal(coreLock?.version, packed.version, 'Core lock version must match the staged SDK')
      assert.equal(coreLock?.integrity, packed.integrity,
        'Core bytes changed: regenerate the tested registry lock before building consumers')
      lock.version = manifest.version
      lock.packages[''].version = manifest.version
      lock.packages[''].devDependencies = manifest.devDependencies
      // Only the explicit staged Core source is substituted. npm retains the
      // other existing locked versions when resolving this ephemeral lock.
      delete lock.packages['node_modules/@esp-vision/device-core']
      await writeFile(join(consumer, 'package-lock.json'), JSON.stringify(lock, null, 2) + '\n')
    } catch (error) {
      if (error.code === 'ENOENT') throw new Error(`${name} requires a tracked registry package-lock.json`)
      throw error
    }
    npm(['install', '--package-lock-only', '--ignore-scripts', '--no-audit', '--no-fund'], consumer)
    npm(['ci', '--ignore-scripts', '--no-audit', '--no-fund'], consumer)
    await rm(join(source, 'node_modules'), { recursive: true, force: true })
    await cp(join(consumer, 'node_modules'), join(source, 'node_modules'), { recursive: true })
    const installed = JSON.parse(await readFile(join(source, 'node_modules/@esp-vision/device-core/package.json'), 'utf8'))
    assert.equal(installed.version, packed.version)
    console.log(`${name}: installed explicit ${packed.name}@${packed.version} (${receipt.sha256})`)
  }
  console.log(`Core npm artifact: ${tarball}`)
} finally {
  await rm(temporary, { recursive: true, force: true })
}

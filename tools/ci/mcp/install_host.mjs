#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// MR integration uses an explicit Core tarball; tracked manifests remain registry-only.
import assert from 'node:assert/strict'
import { minorRange, inMinorRange } from '../../../host/device-core/scripts/version-policy.mjs'
import { createHash } from 'node:crypto'
import { cp, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

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
function dependencyClosure(packages, starts) {
  const reached = new Set()
  const pending = [...starts]
  while (pending.length) {
    const path = pending.pop()
    if (reached.has(path) || !packages[path]) continue
    reached.add(path)
    const entry = packages[path]
    for (const name of Object.keys({ ...entry.dependencies, ...entry.optionalDependencies, ...entry.peerDependencies })) {
      let parent = path
      while (true) {
        const candidate = parent ? `${parent}/node_modules/${name}` : `node_modules/${name}`
        if (packages[candidate]) { pending.push(candidate); break }
        if (!parent) break
        const index = parent.lastIndexOf('/node_modules/')
        parent = index < 0 ? '' : parent.slice(0, index)
      }
    }
  }
  return reached
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
    const pinnedCore = manifest.devDependencies?.['@esp-vision/device-core']
    minorRange(pinnedCore)
    const trackedLock = JSON.parse(await readFile(join(source, 'package-lock.json'), 'utf8'))
    const lockedCore = trackedLock.packages?.['node_modules/@esp-vision/device-core']?.version
    assert.ok(inMinorRange(pinnedCore, lockedCore), 'Core lock must satisfy the declared minor range')
    assert.equal(manifest.espVision?.deviceCore, pinnedCore, 'CLI Core metadata must match its dependency range')
    assert.equal(manifest.espVision?.resolved?.deviceCore, lockedCore, 'CLI metadata must identify the locked Core')
    if (lockedCore !== packed.version) {
      // Core and CLI release independently. Keep testing the CLI against its
      // published, locked Core until the CLI explicitly updates its pin.
      const lock = JSON.parse(await readFile(join(source, 'package-lock.json'), 'utf8'))
      const entry = lock.packages?.['node_modules/@esp-vision/device-core']
      assert.equal(entry?.version, lockedCore, 'Core lock must match the selected version')
      assert.equal(new URL(entry.resolved).origin, 'https://registry.npmjs.org', 'Core lock must use the npm registry')
      assert.match(entry.integrity ?? '', /^sha512-[A-Za-z0-9+/]+={0,2}$/, 'Core lock must include integrity')
      npm(['ci', '--ignore-scripts', '--no-audit', '--no-fund'], source)
      const installed = JSON.parse(await readFile(join(source, 'node_modules/@esp-vision/device-core/package.json'), 'utf8'))
      assert.equal(installed.version, lockedCore)
      console.log(`${name}: installed locked published Core ${lockedCore}; source Core ${packed.version} is independent`)
      continue
    }
    const consumer = join(temporary, name)
    await mkdir(consumer)
    manifest.devDependencies['@esp-vision/device-core'] = `file:${tarball}`
    await writeFile(join(consumer, 'package.json'), JSON.stringify(manifest, null, 2) + '\n')
    let lockedPackages
    try {
      const lock = JSON.parse(await readFile(join(source, 'package-lock.json'), 'utf8'))
      const coreLock = lock.packages['node_modules/@esp-vision/device-core']
      assert.equal(coreLock?.version, packed.version, 'Core lock version must match the staged SDK')
      if (coreLock.resolved === undefined && coreLock.integrity === undefined) {
        // A coordinated new minor can be tested before its Core is published.
        // No registry URL/hash is invented: only this exact source tarball is
        // allowed here. Publication still requires a registry integrity lock.
        assert.equal(manifest.espVision.resolved.deviceCore, packed.version)
        console.log(`${name}: qualifying unpublished Core source; registry lock must be completed before publication`)
      } else {
        assert.equal(new URL(coreLock.resolved).origin, 'https://registry.npmjs.org',
          'The tracked Core lock must retain its published registry source')
        assert.match(coreLock.integrity, /^sha512-[A-Za-z0-9+/]+={0,2}$/,
          'The tracked Core lock must retain its published archive integrity')
      }
      lock.version = manifest.version
      lock.packages[''].version = manifest.version
      lock.packages[''].devDependencies = manifest.devDependencies
      // Core can replace its own transitive dependencies (for example its ROM
      // adapter). Keep every unrelated lock entry, including packages shared
      // with another host dependency, protected against resolver changes.
      const corePath = 'node_modules/@esp-vision/device-core'
      const corePackages = dependencyClosure(lock.packages, [corePath])
      const rootEntry = lock.packages['']
      const hostStarts = Object.keys({ ...rootEntry.dependencies, ...rootEntry.devDependencies,
        ...rootEntry.optionalDependencies, ...rootEntry.peerDependencies })
        .filter(name => name !== '@esp-vision/device-core').map(name => `node_modules/${name}`)
      const hostPackages = dependencyClosure(lock.packages, hostStarts)
      delete lock.packages['node_modules/@esp-vision/device-core']
      lockedPackages = structuredClone(lock.packages)
      delete lockedPackages['']
      for (const path of corePackages) {
        if (!hostPackages.has(path)) delete lockedPackages[path]
      }
      await writeFile(join(consumer, 'package-lock.json'), JSON.stringify(lock, null, 2) + '\n')
    } catch (error) {
      if (error.code === 'ENOENT') throw new Error(`${name} requires a tracked registry package-lock.json`)
      throw error
    }
    npm(['install', '--package-lock-only', '--ignore-scripts', '--no-audit', '--no-fund'], consumer)
    const stagedLock = JSON.parse(await readFile(join(consumer, 'package-lock.json'), 'utf8'))
    const stagedCore = stagedLock.packages['node_modules/@esp-vision/device-core']
    assert.equal(stagedCore?.version, packed.version, 'The temporary lock must select the staged Core version')
    assert.equal(stagedCore?.integrity, packed.integrity, 'The temporary lock must verify the staged Core bytes')
    assert.ok(stagedCore?.resolved?.startsWith('file:'), 'The temporary lock must use a local Core artifact')
    assert.equal(fileURLToPath(new URL(stagedCore.resolved, pathToFileURL(`${consumer}/`))), tarball,
      'The temporary lock must use the explicit Core artifact')
    for (const [path, locked] of Object.entries(lockedPackages)) {
      assert.deepEqual(stagedLock.packages[path], locked, `Unrelated locked dependency changed: ${path}`)
    }
    npm(['ci', '--ignore-scripts', '--no-audit', '--no-fund'], consumer)
    await rm(join(source, 'node_modules'), { recursive: true, force: true })
    await cp(join(consumer, 'node_modules'), join(source, 'node_modules'), { recursive: true })
    const installed = JSON.parse(await readFile(join(source, 'node_modules/@esp-vision/device-core/package.json'), 'utf8'))
    assert.equal(installed.version, packed.version)
    for (const { path } of packed.files) {
      const original = await readFile(join(core, path))
      const actual = await readFile(join(source, 'node_modules/@esp-vision/device-core', path))
      assert.ok(original.equals(actual), `Installed Core differs from the staged source: ${path}`)
    }
    console.log(`${name}: installed explicit ${packed.name}@${packed.version} (${receipt.sha256})`)
  }
  console.log(`Core npm artifact: ${tarball}`)
} finally {
  await rm(temporary, { recursive: true, force: true })
}

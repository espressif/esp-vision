// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Complete an unpublished Core pin only after its protected-branch publication.
import assert from 'node:assert/strict'
import { readFile, writeFile } from 'node:fs/promises'
import { resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { inMinorRange, minorRange } from '../../../host/device-core/scripts/version-policy.mjs'

export function resolveCoreReleaseLock(manifest, lock, metadata) {
  const entry = lock.packages?.['node_modules/@esp-vision/device-core']
  const range = manifest.devDependencies?.['@esp-vision/device-core']
  minorRange(range)
  assert.ok(inMinorRange(range, entry?.version), 'Core lock is outside the declared minor range')
  assert.equal(manifest.espVision?.deviceCore, range)
  assert.equal(manifest.espVision?.resolved?.deviceCore, entry.version)
  assert.equal(metadata.name, '@esp-vision/device-core')
  assert.equal(metadata.version, entry.version, 'Registry Core version differs from the lock')
  assert.match(metadata.dist?.integrity ?? '', /^sha512-[A-Za-z0-9+/]{86}==$/, 'Registry must provide SHA-512 integrity')
  const url = new URL(metadata.dist?.tarball)
  assert.equal(url.origin, 'https://registry.npmjs.org', 'Core tarball must come from npm')
  assert.ok(!url.username && !url.password && !url.search && !url.hash)
  assert.equal(url.pathname, `/@esp-vision/device-core/-/device-core-${entry.version}.tgz`)
  if (entry.integrity !== undefined || entry.resolved !== undefined) {
    assert.equal(entry.integrity, metadata.dist.integrity, 'Published Core differs from the locked bytes')
    assert.equal(entry.resolved, metadata.dist.tarball, 'Published Core archive differs from the lock')
  }
  const result = structuredClone(lock)
  Object.assign(result.packages['node_modules/@esp-vision/device-core'], {
    resolved: metadata.dist.tarball, integrity: metadata.dist.integrity,
  })
  return result
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const root = fileURLToPath(new URL('../../../host/device-cli/', import.meta.url))
  const manifest = JSON.parse(await readFile(resolve(root, 'package.json'), 'utf8'))
  const lockPath = resolve(root, 'package-lock.json')
  const lock = JSON.parse(await readFile(lockPath, 'utf8'))
  const version = lock.packages?.['node_modules/@esp-vision/device-core']?.version
  const response = await fetch(`https://registry.npmjs.org/@esp-vision%2fdevice-core/${encodeURIComponent(version)}`, {
    signal: AbortSignal.timeout(30000), cache: 'no-store',
  })
  assert.equal(response.status, 200, `Publish Core ${version} before building the release CLI`)
  const completed = resolveCoreReleaseLock(manifest, lock, await response.json())
  await writeFile(lockPath, JSON.stringify(completed, null, 2) + '\n')
  console.log(`Resolved published Core ${version}; npm ci will verify its archive integrity.`)
}

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import test from 'node:test'
import { resolveCoreReleaseLock } from './resolve_core_release_lock.mjs'

function fixture() {
  return {
    manifest: { devDependencies: { '@esp-vision/device-core': '~0.8.0' },
      espVision: { deviceCore: '~0.8.0', resolved: { deviceCore: '0.8.0' } } },
    lock: { packages: {
      'node_modules/@esp-vision/device-core': { version: '0.8.0', dev: true, dependencies: { koffi: '2.16.3' } },
      'node_modules/koffi': { version: '2.16.3', integrity: 'preserve-existing-pin' },
    } },
    metadata: { name: '@esp-vision/device-core', version: '0.8.0', dist: {
      tarball: 'https://registry.npmjs.org/@esp-vision/device-core/-/device-core-0.8.0.tgz',
      integrity: 'sha512-' + createHash('sha512').update('published bytes').digest('base64'),
    } },
  }
}

test('completes only the missing Core archive pin and preserves the dependency closure', () => {
  const { manifest, lock, metadata } = fixture()
  const result = resolveCoreReleaseLock(manifest, lock, metadata)
  const expected = structuredClone(lock)
  Object.assign(expected.packages['node_modules/@esp-vision/device-core'], {
    resolved: metadata.dist.tarball, integrity: metadata.dist.integrity,
  })
  assert.deepEqual(result, expected)
  assert.equal(lock.packages['node_modules/@esp-vision/device-core'].integrity, undefined)
  assert.deepEqual(resolveCoreReleaseLock(manifest, result, metadata), result)
})

test('rejects registry identity, version, digest and archive substitutions', () => {
  for (const mutate of [
    f => { f.metadata.name = 'imposter' },
    f => { f.metadata.version = '0.8.1' },
    f => { f.metadata.dist.integrity = 'sha512-invalid' },
    f => { f.metadata.dist.tarball = 'https://example.org/core.tgz' },
    f => { f.metadata.dist.tarball = 'https://registry.npmjs.org/@esp-vision/device-core/-/device-core-0.8.1.tgz' },
    f => { f.manifest.espVision.resolved.deviceCore = '0.8.1' },
    f => { f.manifest.devDependencies['@esp-vision/device-core'] = '~0.7.0' },
  ]) {
    const f = fixture()
    mutate(f)
    assert.throws(() => resolveCoreReleaseLock(f.manifest, f.lock, f.metadata))
  }
})

test('rejects partial pins and never overwrites already pinned bytes', () => {
  for (const pin of [
    { integrity: fixture().metadata.dist.integrity },
    { resolved: fixture().metadata.dist.tarball },
    { resolved: fixture().metadata.dist.tarball, integrity: 'sha512-other' },
  ]) {
    const f = fixture()
    Object.assign(f.lock.packages['node_modules/@esp-vision/device-core'], pin)
    assert.throws(() => resolveCoreReleaseLock(f.manifest, f.lock, f.metadata))
  }
})

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { test } from 'node:test'
import { cliBundlesCore, declaredCoreSupports, highestStableVersion, inMinorRange, minorRange, validateVersionStep } from '../scripts/version-policy.mjs'

test('minor dependency ranges accept patch updates and reject adjacent minor or major versions', () => {
  for (const version of ['0.5.0', '0.5.1', '0.5.99']) assert.equal(inMinorRange('~0.5.0', version), true)
  for (const version of ['0.4.9', '0.6.0', '1.5.0']) assert.equal(inMinorRange('~0.5.0', version), false)
  for (const value of ['^0.5.0', '0.5.1', '~0.5.1', '*', '~0.05.0']) assert.throws(() => minorRange(value))
  assert.equal(declaredCoreSupports('0.5.1', '0.5.1'), true)
  assert.equal(declaredCoreSupports('0.5.1', '0.5.2'), false)
  assert.equal(declaredCoreSupports('~0.5.0', '0.5.2'), true)
})

test('only the next patch, minor or major version matches its declared release type', () => {
  for (const [kind, next] of [['patch', '0.5.2'], ['minor', '0.6.0'], ['major', '1.0.0']]) {
    assert.doesNotThrow(() => validateVersionStep('0.5.1', next, kind))
  }
  for (const [kind, next] of [['patch', '0.6.0'], ['patch', '0.5.3'], ['minor', '0.5.2'], ['minor', '0.7.0'], ['major', '0.6.0'], ['major', '1.1.0'], ['feature', '0.6.0']]) {
    assert.throws(() => validateVersionStep('0.5.1', next, kind))
  }
  assert.throws(() => validateVersionStep('0.5.1', '0.5.2-rc.1', 'patch'))
})

test('registry baseline is the numerically highest stable published version', () => {
  assert.equal(highestStableVersion(['0.9.9', '0.10.1', '0.10.0', '0.11.0-beta.1', '0.0.0-stage', '1.0.0']), '1.0.0')
  assert.equal(highestStableVersion(['0.0.0-stage']), undefined)
})

test('a bundled CLI identifies the actual Core patch even when its range allows others', () => {
  const cli = { devDependencies: { '@esp-vision/device-core': '~0.5.0' }, espVision: { resolved: { deviceCore: '0.5.1' } } }
  assert.equal(cliBundlesCore(cli, '0.5.1'), true)
  assert.equal(cliBundlesCore(cli, '0.5.2'), false)
  assert.equal(cliBundlesCore({ devDependencies: { '@esp-vision/device-core': '0.5.1' } }, '0.5.1'), true)
  assert.equal(cliBundlesCore({ devDependencies: { '@esp-vision/device-core': '~0.5.0' } }, '0.5.1'), false)
})

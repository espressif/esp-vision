// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { test } from 'node:test'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { knowledgeVersion, fallbackKnowledgeVersion } from '../skills/esp-vision-development/scripts/knowledge-version.mjs'

const inspection = version => ({ ok: true, command: 'inspect', result: { identity: { firmwareVersion: version } } })
const snapshots = [
  { firmwareVersion: '2026.11.06', snapshotVersion: '2026.11.06-docs2', active: true },
  { firmwareVersion: '2026.11.06', snapshotVersion: '2026.11.06', active: false },
  { firmwareVersion: '2026.12.01-dirty', snapshotVersion: 'dev', active: true },
  { firmwareVersion: '2026.11.31', snapshotVersion: 'invalid-date', active: true },
  { firmwareVersion: '2026.12.02', snapshotVersion: 'inactive', active: false },
  { firmwareVersion: '2026.10.21', snapshotVersion: '2026.10.21', active: true },
]
const catalog = { latestFirmwareVersion: '2026.11.06', firmwareVersions: snapshots.map(snapshot => snapshot.firmwareVersion), snapshots }
test('fallback requires an explicit decline and pins the latest available official active snapshot', () => {
  for (const updateDeclined of [undefined, false, 'true', 'false', 1]) {
    assert.throws(() => fallbackKnowledgeVersion(catalog, { updateDeclined }), /FIRMWARE_UPDATE_CHOICE_REQUIRED/)
  }
  assert.deepEqual(fallbackKnowledgeVersion(catalog, { updateDeclined: true }), {
    firmwareVersion: '2026.11.06', snapshotVersion: '2026.11.06-docs2',
  })
  // A previous approved call does not authorize another device/task.
  assert.throws(() => fallbackKnowledgeVersion(catalog), /FIRMWARE_UPDATE_CHOICE_REQUIRED/)
  assert.throws(() => knowledgeVersion(inspection('unknown')), /FIRMWARE_VERSION_REQUIRED/)
})
test('unavailable or ambiguous catalogs never produce a guessed fallback', () => {
  for (const value of [null, {}, { snapshots: [] }]) {
    assert.throws(() => fallbackKnowledgeVersion(value, { updateDeclined: true }), /KNOWLEDGE_CATALOG_REQUIRED/)
  }
  for (const value of [{ firmwareVersions: [], snapshots: [] },
    { firmwareVersions: [], snapshots },
    { firmwareVersions: catalog.firmwareVersions, snapshots: snapshots.filter(snapshot => !['2026.11.06', '2026.10.21'].includes(snapshot.firmwareVersion)) }]) {
    assert.throws(() => fallbackKnowledgeVersion(value, { updateDeclined: true }), /KNOWLEDGE_VERSION_UNAVAILABLE/)
  }
  assert.throws(() => fallbackKnowledgeVersion({ ...catalog, snapshots: [...snapshots, snapshots[0]] },
    { updateDeclined: true }), /KNOWLEDGE_CATALOG_INVALID/)
})
test('explicit latest identity supports unpadded dates, semver and mixed release histories without rewriting tags', () => {
  for (const firmwareVersion of ['2026.11.6', 'v1.2', 'v1.2.3', '2026.11.06-1']) {
    const current = { ...catalog, latestFirmwareVersion: firmwareVersion,
      firmwareVersions: [...catalog.firmwareVersions, firmwareVersion],
      snapshots: [...snapshots, { firmwareVersion, snapshotVersion: firmwareVersion + '-docs2', active: true }] }
    assert.deepEqual(fallbackKnowledgeVersion(current, { updateDeclined: true }), {
      firmwareVersion, snapshotVersion: firmwareVersion + '-docs2',
    })
  }
  assert.throws(() => fallbackKnowledgeVersion({ ...catalog, latestFirmwareVersion: '2026.12.02' },
    { updateDeclined: true }), /KNOWLEDGE_CATALOG_INVALID/)
  assert.throws(() => fallbackKnowledgeVersion({ ...catalog, latestFirmwareVersion: undefined },
    { updateDeclined: true }), /KNOWLEDGE_VERSION_UNAVAILABLE/)
})
test('a literal latest alias cannot become an approved fallback even in a forged catalog', () => {
  assert.throws(() => fallbackKnowledgeVersion({ latestFirmwareVersion: 'latest', firmwareVersions: ['latest'],
    snapshots: [{ firmwareVersion: 'latest', snapshotVersion: 'latest', active: true }] },
  { updateDeclined: true }), /KNOWLEDGE_VERSION_UNAVAILABLE/)
})
test('version matching preserves exact firmware identity including development suffixes', () => {
  for (const version of ['2026.10.21', '2026.11.06', '2026.11.06-14-gabcdef-dirty', 'v1.2.3']) {
    assert.deepEqual(knowledgeVersion(inspection(version)), { firmwareVersion: version })
  }
})
test('missing identity, passive discovery, unknown version and failed inspection never select latest', () => {
  for (const value of [{}, { ok: false }, inspection(null), inspection(''), inspection('unknown'), inspection('latest'),
    { ok: true, command: 'inspect', result: { devices: [{ identity: { firmwareVersion: '2026.11.06' } }] } }]) {
    assert.throws(() => knowledgeVersion(value), /FIRMWARE_VERSION_REQUIRED/)
  }
})
test('shipped helper consumes the actual CLI JSON envelope on stdin', () => {
  const helper = fileURLToPath(new URL('../skills/esp-vision-development/scripts/knowledge-version.mjs', import.meta.url))
  const result = spawnSync(process.execPath, [helper], { input: JSON.stringify(inspection('2026.10.21')), encoding: 'utf8' })
  assert.equal(result.status, 0, result.stderr)
  assert.deepEqual(JSON.parse(result.stdout), { firmwareVersion: '2026.10.21' })
  const missing = spawnSync(process.execPath, [helper], { input: '{}', encoding: 'utf8' })
  assert.notEqual(missing.status, 0)
  assert.equal(missing.stdout, '')
  const fallback = spawnSync(process.execPath, [helper, '--declined-firmware-update'], { input: JSON.stringify(catalog), encoding: 'utf8' })
  assert.equal(fallback.status, 0, fallback.stderr)
  assert.deepEqual(JSON.parse(fallback.stdout), { firmwareVersion: '2026.11.06', snapshotVersion: '2026.11.06-docs2' })
  assert.match(fallback.stderr, /KNOWLEDGE_VERSION_MISMATCH.*compatibility is unverified/)
  const unapproved = spawnSync(process.execPath, [helper], { input: JSON.stringify(catalog), encoding: 'utf8' })
  assert.notEqual(unapproved.status, 0)
  assert.equal(unapproved.stdout, '')
  const invalidFlag = spawnSync(process.execPath, [helper, '--latest'], { input: JSON.stringify(catalog), encoding: 'utf8' })
  assert.notEqual(invalidFlag.status, 0)
  assert.equal(invalidFlag.stdout, '')
})

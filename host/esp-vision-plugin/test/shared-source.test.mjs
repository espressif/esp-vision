// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { cp, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { test } from 'node:test'
import { fileURLToPath } from 'node:url'
import { buildCursorArchive } from '../scripts/cursor.mjs'
import { buildQoderArchive } from '../scripts/qoderwork.mjs'
import { buildExpertArchive } from '../scripts/workbuddy-expert.mjs'
import { sharedMetadata } from '../scripts/export-common.mjs'
import { syncHostManifests } from '../scripts/host-manifests.mjs'
import { installPlugin } from '../src/installer.mjs'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const normalize = text => text.replaceAll('@references/application-workflow.md', '[application workflow](references/application-workflow.md)')
function businessBody(skill) {
  assert.match(skill, /Each CLI operation connects/, 'Common device policy is missing')
  return normalize(skill.replace(/^---\n[\s\S]*?\n---/, ''))
    .split('\n\n').filter(paragraph => !paragraph.startsWith('Invoke the installed plugin copy with Node') && !paragraph.startsWith('Invoke Node with'))
    .join('\n\n').replace(/Use `node [^`]+ --help` for command syntax\./, 'Use [installed CLI help] for command syntax.')
}

function inspect(archive) {
  return JSON.parse(execFileSync('python3', ['-c', 'import sys,json,zipfile; z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None; print(json.dumps({n:z.read(n).decode() for n in z.namelist() if n.endswith("/SKILL.md") or n.endswith("/plugin.json") or n.endswith("/application-workflow.md")}))', archive], { encoding: 'utf8' }))
}

test('all five hosts receive the same changed device policy and canonical publisher identity', async t => {
  const base = resolve(process.env.TMPDIR ?? resolve(root, '../../.cache/shared-source-tests'))
  await mkdir(base, { recursive: true })
  const temp = await mkdtemp(resolve(base, 'shared-source-'))
  t.after(() => rm(temp, { recursive: true, force: true }))
  const source = resolve(temp, 'source')
  await cp(root, source, { recursive: true })
  const common = JSON.parse(await readFile(resolve(source, 'plugin.json'), 'utf8'))
  common.homepage = 'https://vision.espressif.com/shared-source-proof'
  common.author.name = 'Shared publisher propagation proof'
  common.author.email = 'shared-source-proof@espressif.com'
  await writeFile(resolve(source, 'plugin.json'), JSON.stringify(common))
  const skillPath = 'skills/esp-vision-development/SKILL.md'
  const sharedSkill = await readFile(resolve(source, skillPath), 'utf8') + '\nShared-source propagation proof: preserve this acceptance rule.\n'
  await writeFile(resolve(source, skillPath), sharedSkill)
  await syncHostManifests(source)
  await syncHostManifests(source, { check: true })
  const driftPath = resolve(source, '.claude-plugin/plugin.json')
  const drift = JSON.parse(await readFile(driftPath, 'utf8'))
  await writeFile(driftPath, JSON.stringify({ ...drift, homepage: 'https://invalid.example/independent-copy' }))
  await assert.rejects(syncHostManifests(source, { check: true }), /is generated; rebuild/)
  await syncHostManifests(source)
  // Native Codex and Claude resolve this one on-disk Skill; their metadata is checked/generated.
  assert.equal(businessBody(await readFile(resolve(source, skillPath), 'utf8')), businessBody(sharedSkill))
  const claude = JSON.parse(await readFile(resolve(source, '.claude-plugin/plugin.json'), 'utf8'))
  assert.equal(claude.homepage, common.homepage)
  assert.deepEqual(claude.author, common.author)
  const env = { CI: 'true', CI_COMMIT_SHA: 'c'.repeat(40), CI_PIPELINE_ID: '42' }
  for (const [name, build] of [['cursor', buildCursorArchive], ['qoder', buildQoderArchive], ['workbuddy', buildExpertArchive]]) {
    const result = await build({ sourceRoot: source, outputDirectory: resolve(temp, name), ciEnvironment: env })
    const files = inspect(result.archivePath)
    const manifest = JSON.parse(Object.entries(files).find(([path]) => path.endsWith('/plugin.json'))[1])
    assert.equal(manifest.homepage, common.homepage, name)
    assert.deepEqual(manifest.author, { name: common.author.name, email: common.author.email }, name)
    const skill = Object.entries(files).find(([path]) => path.endsWith('/SKILL.md'))[1]
    assert.equal(businessBody(skill), businessBody(sharedSkill), name)
  }
  const legacy = await installPlugin({ client: 'workbuddy', packageRoot: source, dataDirectory: resolve(temp, 'legacy-data') })
  const legacySkill = Object.entries(inspect(legacy.skillZip)).find(([path]) => path.endsWith('/SKILL.md'))[1]
  assert.ok(legacySkill.includes(JSON.stringify(common.author.name)))
  assert.equal(businessBody(legacySkill), businessBody(sharedSkill), 'legacy WorkBuddy variant')
})

test('platform overlays cannot silently redefine shared identity', () => {
  for (const key of ['name', 'author', 'homepage', 'repository', 'license', 'keywords']) {
    assert.throws(() => sharedMetadata({}, { [key]: 'independent copy' }), /cannot redefine shared identity/)
  }
})

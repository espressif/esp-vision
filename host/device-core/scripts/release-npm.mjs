// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { verifyPublishedIntegrity } from './verify-registry.mjs'
import { spawnSync } from 'node:child_process'
import { mkdtemp, readFile, writeFile, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const registry = 'https://registry.npmjs.org/'
const dryRun = process.argv.includes('--dry-run')
assert.ok(process.argv.slice(2).every(arg => arg === '--dry-run'), 'Unknown release argument')
const manifest = JSON.parse(await readFile(join(root, 'package.json'), 'utf8'))
assert.equal(manifest.name, '@esp-vision/device-core')
assert.match(manifest.version, /^\d+\.\d+\.\d+$/)
assert.equal(manifest.publishConfig?.registry, registry)
assert.equal(manifest.publishConfig?.access, 'public')
assert.equal(manifest.license, 'Apache-2.0')
if (!dryRun) {
  assert.equal(process.env.CI_PIPELINE_SOURCE, 'push', 'Publish only from the merged default-branch pipeline')
  assert.ok(process.env.CI_DEFAULT_BRANCH && process.env.CI_COMMIT_BRANCH === process.env.CI_DEFAULT_BRANCH,
    'Publish only after manual merge to the default branch')
  assert.equal(process.env.CI_COMMIT_REF_PROTECTED, 'true', 'Publishing requires a protected branch')
  assert.ok(process.env.NPM_PUBLISH_TOKEN, 'Protected NPM_PUBLISH_TOKEN is missing')
}
const temporary = await mkdtemp(join(tmpdir(), 'esp-vision-core-release-'))
try {
  // Store an environment reference, never the token value, outside the package.
  const config = join(temporary, 'npmrc')
  await writeFile(config, `registry=${registry}\n//registry.npmjs.org/:_authToken=\${NPM_PUBLISH_TOKEN}\n`, { mode: 0o600 })
  const env = dryRun ? { ...process.env } : { ...process.env, NPM_CONFIG_USERCONFIG: config }
  function npm(args) {
    const result = spawnSync('npm', args, { cwd: root, env, encoding: 'utf8' })
    if (result.status !== 0) throw new Error(`npm ${args[0]} failed: ${result.stderr || result.error?.message}`)
    return result.stdout
  }
  const [packed] = JSON.parse(npm(['pack', '--json', '--ignore-scripts', '--pack-destination', temporary]))
  for (const required of ['LICENSE', 'NOTICE']) assert.ok(packed.files.some(({ path }) => path === required), `Missing ${required}`)
  assert.ok(packed.files.every(({ path }) => /^(src\/|schemas\/|tests\/|scripts\/|package\.json$|README\.md$|LICENSE$|NOTICE$|CHANGELOG\.md$)/.test(path)),
    'Only the standalone library may be published')
  if (dryRun) {
    npm(['publish', join(temporary, packed.filename), '--dry-run', '--ignore-scripts', '--access', 'public', '--registry', registry])
    console.log(`Dry run: ${packed.name}@${packed.version}; ${packed.files.length} files; no registry write`)
  } else {
    const publisher = npm(['whoami', '--registry', registry]).trim()
    assert.equal(publisher, 'esp-vision', 'Npm publisher account must be esp-vision')
    console.log(`Npm publisher: ${publisher}`)
    const url = new URL(encodeURIComponent(manifest.name), registry)
    url.pathname += '/' + encodeURIComponent(manifest.version)
    const response = await fetch(url, { cache: 'no-store', headers: { 'Cache-Control': 'no-cache' }, signal: AbortSignal.timeout(15000) })
    if (response.ok) {
      const existing = await response.json()
      assert.equal(existing.dist?.integrity, packed.integrity,
        'This version already exists with different content; bump the package version')
      console.log(`${packed.name}@${packed.version} already published with identical content; skipped`)
    } else {
      assert.equal(response.status, 404, 'Registry lookup failed; do not attempt publication')
      npm(['publish', join(temporary, packed.filename), '--ignore-scripts', '--access', 'public', '--registry', registry])
      console.log('npm publish accepted; verifying registry metadata')
      await verifyPublishedIntegrity(url, packed.integrity)
      console.log(`Published and verified ${packed.name}@${packed.version}`)
    }
  }
} finally {
  await rm(temporary, { recursive: true, force: true })
}

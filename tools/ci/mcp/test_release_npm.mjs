// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { chmod, copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { test } from 'node:test'

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../../..')
const helper = join(repoRoot, 'tools/ci/mcp/release_npm.mjs')
const tempBase = resolve(process.env.ESP_VISION_NPM_TEMP_DIR ?? process.env.TMPDIR ?? join(repoRoot, '.cache/npm-release'))

async function createReleaseFixture(prefix = 'release-policy-') {
  await mkdir(tempBase, { recursive: true })
  const root = await mkdtemp(join(tempBase, prefix))
  const fixtureHelper = join(root, 'tools/ci/mcp/release_npm.mjs')
  await mkdir(dirname(fixtureHelper), { recursive: true })
  await copyFile(helper, fixtureHelper)

  const packages = [
    ['host/device-cli', '@esp-vision/device-cli'],
    ['host/knowledge-mcp', '@esp-vision/knowledge-mcp'],
    ['host/esp-vision-plugin', '@esp-vision/plugin'],
  ]
  for (const [relativePath, name] of packages) {
    const packageRoot = join(root, relativePath)
    await mkdir(packageRoot, { recursive: true })
    await writeFile(join(packageRoot, 'package.json'), JSON.stringify({
      name,
      version: '0.1.0',
      license: 'Apache-2.0',
      publishConfig: { access: 'public', registry: 'https://registry.npmjs.org/' },
      dependencies: {},
    }))
  }

  const temporary = join(root, 'temporary')
  await mkdir(temporary, { recursive: true })
  return {
    root,
    helper: fixtureHelper,
    env: {
      ...process.env,
      TMPDIR: temporary,
      TMP: temporary,
      TEMP: temporary,
      ESP_VISION_NPM_TEMP_DIR: join(temporary, 'release'),
      ESP_VISION_NPM_TARBALL_DIR: join(temporary, 'tarballs'),
    },
  }
}

async function withReleaseFixture(callback, prefix) {
  const fixture = await createReleaseFixture(prefix)
  try {
    await callback(fixture)
  } finally {
    await rm(fixture.root, { recursive: true, force: true })
  }
}

function runHelper(fixture, args, overrides = fixture.env) {
  const env = { ...overrides }
  const result = spawnSync(process.execPath, [fixture.helper, ...args], {
    cwd: fixture.root,
    env,
    encoding: 'utf8',
    maxBuffer: 2 * 1024 * 1024,
  })
  return { ...result, output: `${result.stdout ?? ''}${result.stderr ?? ''}` }
}

function withoutTokenEnv(base = process.env, overrides = {}) {
  const env = { ...base, ...overrides }
  delete env.NPM_PUBLISH_TOKEN
  return env
}

test('release helper accepts only explicitly supported bundled packages', () => {
  return withReleaseFixture(async (fixture) => {
    const result = runHelper(fixture, ['--dry-run', '--package', 'host/device-core'])
    assert.notEqual(result.status, 0)
    assert.match(result.output, /exactly host\/device-cli, host\/knowledge-mcp or host\/esp-vision-plugin/)
  })
})

test('release helper rejects a local file dependency before running npm', () => {
  return withReleaseFixture(async (fixture) => {
    const fixturePackage = join(fixture.root, 'host/device-cli/package.json')
    const manifest = JSON.parse(await readFile(fixturePackage, 'utf8'))
    manifest.devDependencies = { 'local-package': 'file:../local-package' }
    await writeFile(fixturePackage, JSON.stringify(manifest))
    const result = runHelper(fixture, ['--dry-run', '--package', 'host/device-cli'], withoutTokenEnv(fixture.env))
    assert.notEqual(result.status, 0)
    assert.match(result.output, /must not refer to a local file/)
  }, 'release-policy-file-dependency-')
})

test('publish mode is blocked outside a protected default-branch push', () => {
  return withReleaseFixture(async (fixture) => {
    const env = withoutTokenEnv(fixture.env, {
      CI_PIPELINE_SOURCE: 'merge_request_event',
      CI_DEFAULT_BRANCH: 'master',
      CI_COMMIT_BRANCH: 'master',
      CI_COMMIT_REF_PROTECTED: 'true',
    })
    const result = runHelper(fixture, ['--package', 'host/device-cli'], env)
    assert.notEqual(result.status, 0)
    assert.match(result.output, /Publish only from the merged default-branch pipeline/)
  })
})

test('publish mode requires the protected npm token before running a build', () => {
  return withReleaseFixture(async (fixture) => {
    const env = withoutTokenEnv(fixture.env, {
      CI_PIPELINE_SOURCE: 'push',
      CI_DEFAULT_BRANCH: 'master',
      CI_COMMIT_BRANCH: 'master',
      CI_COMMIT_REF_PROTECTED: 'true',
    })
    const result = runHelper(fixture, ['--package', 'host/device-cli'], env)
    assert.notEqual(result.status, 0)
    assert.match(result.output, /Protected NPM_PUBLISH_TOKEN is missing/)
  })
})

test('helper redacts token-like text from npm diagnostics', () => {
  return withReleaseFixture(async (fixture) => {
    const fakeBin = join(fixture.root, 'bin')
    const fakeNpm = join(fakeBin, 'npm')
    await mkdir(fakeBin, { recursive: true })
    await writeFile(fakeNpm, '#!/usr/bin/env node\nprocess.stderr.write(process.env.NPM_PUBLISH_TOKEN)\nprocess.exit(42)\n')
    await chmod(fakeNpm, 0o755)
    const canary = 'npm-token-canary-must-not-be-printed'
    const env = {
      ...fixture.env,
      PATH: `${fakeBin}:${process.env.PATH ?? ''}`,
      NPM_PUBLISH_TOKEN: canary,
    }
    const result = runHelper(fixture, ['--dry-run', '--package', 'host/device-cli'], env)
    assert.notEqual(result.status, 0)
    assert.match(result.output, /\[redacted\]/)
    assert.doesNotMatch(result.output, new RegExp(canary))
  }, 'release-policy-token-output-')
})


test('Knowledge publication rejects a default-branch push without a release tag', () => {
  return withReleaseFixture(async (fixture) => {
    const result = runHelper(fixture, ['--package', 'host/knowledge-mcp'], withoutTokenEnv(fixture.env, {
      CI_PIPELINE_SOURCE: 'push', CI_COMMIT_TAG: '', CI_DEFAULT_BRANCH: 'master',
      CI_COMMIT_BRANCH: 'master', CI_COMMIT_REF_PROTECTED: 'true',
    }))
    assert.notEqual(result.status, 0)
    assert.match(result.output, /requires a protected firmware release tag/)
  })
})

test('Knowledge publication requires the exact checked tarball before token use', () => {
  return withReleaseFixture(async (fixture) => {
    const result = runHelper(fixture, ['--package', 'host/knowledge-mcp'], withoutTokenEnv(fixture.env, {
      CI_PIPELINE_SOURCE: 'push', CI_COMMIT_TAG: '2026.10.22',
      CI_COMMIT_REF_PROTECTED: 'true',
    }))
    assert.notEqual(result.status, 0)
    assert.match(result.output, /exact Knowledge tarball used by the image/)
  })
})

test('Knowledge release tags still require protected credentials', () => {
  return withReleaseFixture(async (fixture) => {
    const result = runHelper(fixture, ['--package', 'host/knowledge-mcp', '--tarball', 'not-read.tgz'], withoutTokenEnv(fixture.env, {
      CI_PIPELINE_SOURCE: 'push', CI_COMMIT_TAG: '2026.10.22',
      CI_COMMIT_REF_PROTECTED: 'false',
    }))
    assert.notEqual(result.status, 0)
    assert.match(result.output, /Publishing requires a protected branch/)
  })
})

test('release helper rejects conflicting supplied tarball options', () => {
  return withReleaseFixture(async (fixture) => {
    const result = runHelper(fixture, [
      '--dry-run', '--package', 'host/knowledge-mcp',
      '--tarball', 'supplied.tgz', '--tarball-dir', 'artifacts',
    ])
    assert.notEqual(result.status, 0)
    assert.match(result.output, /Choose --tarball or --tarball-dir/)
  })
})

async function archivedCliFixture(fixture) {
  const packageRoot = join(fixture.root, 'host/device-cli')
  const manifest = JSON.parse(await readFile(join(packageRoot, 'package.json'), 'utf8'))
  Object.assign(manifest, { files: ['bin'], bin: { 'fixture-cli': 'bin/fixture.mjs' } })
  await writeFile(join(packageRoot, 'package.json'), JSON.stringify(manifest))
  const staged = join(fixture.root, 'staged')
  await mkdir(join(staged, 'package/bin'), { recursive: true })
  await writeFile(join(staged, 'package/package.json'), JSON.stringify(manifest))
  await writeFile(join(staged, 'package/bin/fixture.mjs'), '#!/usr/bin/env node\n')
  await chmod(join(staged, 'package/bin/fixture.mjs'), 0o755)
  await mkdir(join(packageRoot, 'scripts'))
  await writeFile(join(packageRoot, 'scripts/check-package.mjs'), 'process.exit(0)\n')
  const archive = join(fixture.root, 'published-version.tgz')
  assert.equal(spawnSync('tar', ['-czf', archive, '-C', staged, 'package']).status, 0)
  const bin = join(fixture.root, 'fake-bin'), log = join(fixture.root, 'npm-calls.jsonl')
  await mkdir(bin)
  const npm = join(bin, 'npm')
  await writeFile(npm, `#!/usr/bin/env node
const fs = require('node:fs'), crypto = require('node:crypto');
const args = process.argv.slice(2);
fs.appendFileSync(process.env.NPM_CALL_LOG, JSON.stringify(args) + "\\n");
if (args[0] === 'publish') {
  process.stderr.write('You cannot publish over the previously published versions: 0.1.0.');
  process.exit(42);
}
if (args[0] === 'whoami') { process.stdout.write('esp-vision'); process.exit(0); }
if (args[0] !== 'pack' || !args.includes('--dry-run')) process.exit(43);
const integrity = 'sha512-' + crypto.createHash('sha512').update(fs.readFileSync(args[1])).digest('base64');
process.stdout.write(JSON.stringify([{ name: '@esp-vision/device-cli', version: '0.1.0', integrity: process.env.BAD_PACK_INTEGRITY ? 'sha512-invalid' : integrity }]));
`)
  await chmod(npm, 0o755)
  const env = withoutTokenEnv(fixture.env, { PATH: `${bin}:${process.env.PATH ?? ''}`, NPM_CALL_LOG: log })
  return { archive, log, env }
}

test('CI archive validation does not publish or probe an already published version', () => withReleaseFixture(async fixture => {
  const { archive, log, env } = await archivedCliFixture(fixture)
  const result = runHelper(fixture, ['--dry-run', '--package', 'host/device-cli', '--tarball', archive], env)
  assert.equal(result.status, 0, result.output)
  assert.match(result.output, /Archive validation verified/)
  const calls = (await readFile(log, 'utf8')).trim().split('\n').map(line => JSON.parse(line))
  assert.equal(calls.length, 1)
  assert.equal(calls[0][0], 'pack')
  assert.ok(calls[0].includes('--dry-run'))
}))

test('CI archive validation rejects a different integrity reported by npm', () => withReleaseFixture(async fixture => {
  const { archive, env } = await archivedCliFixture(fixture)
  const result = runHelper(fixture, ['--dry-run', '--package', 'host/device-cli', '--tarball', archive], { ...env, BAD_PACK_INTEGRITY: '1' })
  assert.notEqual(result.status, 0)
  assert.match(result.output, /npm interpreted a different tarball/)
}))

test('real publication still rejects changed public bytes and skips identical versions', () => withReleaseFixture(async fixture => {
  const { archive, log, env } = await archivedCliFixture(fixture)
  const loader = join(fixture.root, 'registry-fixture.mjs')
  await writeFile(loader, `globalThis.fetch = async () => ({ok:true,status:200,json:async()=>({dist:{integrity:process.env.REGISTRY_INTEGRITY}})});`)
  const publishEnv = { ...env, NODE_OPTIONS: `--import ${pathToFileURL(loader).href}`, NPM_PUBLISH_TOKEN: 'protected-fixture-token',
    CI_PIPELINE_SOURCE: 'push', CI_DEFAULT_BRANCH: 'master', CI_COMMIT_BRANCH: 'master', CI_COMMIT_REF_PROTECTED: 'true', REGISTRY_INTEGRITY: 'sha512-different' }
  const args = ['--package', 'host/device-cli', '--tarball', archive]
  const changed = runHelper(fixture, args, publishEnv)
  assert.notEqual(changed.status, 0)
  assert.match(changed.output, /already exists with different content/)
  const { createHash } = await import('node:crypto')
  publishEnv.REGISTRY_INTEGRITY = 'sha512-' + createHash('sha512').update(await readFile(archive)).digest('base64')
  const identical = runHelper(fixture, args, publishEnv)
  assert.equal(identical.status, 0, identical.output)
  assert.match(identical.output, /already published with identical content; skipped/)
  const calls = (await readFile(log, 'utf8')).trim().split('\n').map(line => JSON.parse(line))
  assert.deepEqual(calls.map(call => call[0]), ['whoami', 'whoami'])
}))

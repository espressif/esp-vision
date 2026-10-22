// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { chmod, copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
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

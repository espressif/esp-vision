// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { chmod, copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { test } from 'node:test'

const source = fileURLToPath(new URL('../scripts/', import.meta.url))
async function fixture(t, published) {
  const root = await mkdtemp(join(tmpdir(), 'core-release-policy-'))
  t.after(() => rm(root, { recursive: true, force: true }))
  const scripts = join(root, 'scripts'), bin = join(root, 'bin'), log = join(root, 'calls')
  await mkdir(scripts); await mkdir(bin)
  await copyFile(join(source, 'release-npm.mjs'), join(scripts, 'release-npm.mjs'))
  await copyFile(join(source, 'verify-registry.mjs'), join(scripts, 'verify-registry.mjs'))
  await writeFile(join(root, 'package.json'), JSON.stringify({ name: '@esp-vision/device-core', version: '1.2.3', license: 'Apache-2.0', publishConfig: { access: 'public', registry: 'https://registry.npmjs.org/' } }))
  const loader = join(root, 'registry.mjs')
  await writeFile(loader, `let reads = 0; globalThis.fetch = async () => { reads++; const found = ${published} || reads >= 3; return {ok:found,status:found?200:404,json:async()=>({dist:{integrity:'sha512-fixture'}})} }`)
  const npm = join(bin, 'npm')
  await writeFile(npm, `#!/usr/bin/env node
const fs = require('node:fs'); const args = process.argv.slice(2); fs.appendFileSync(process.env.CALL_LOG, args[0] + '\\n');
if (args[0] === 'whoami') process.stdout.write('esp-vision');
else if (args[0] === 'pack') process.stdout.write(JSON.stringify([{name:'@esp-vision/device-core',version:'1.2.3',filename:'core.tgz',integrity:'sha512-fixture',files:[{path:'LICENSE'},{path:'NOTICE'}]}]));
else if (args[0] !== 'publish') process.exit(2);
`)
  await chmod(npm, 0o755)
  const env = { ...process.env, PATH: `${bin}:${process.env.PATH}`, TMPDIR: root, TMP: root, TEMP: root, CALL_LOG: log,
    NODE_OPTIONS: `--import ${pathToFileURL(loader).href}`, NPM_PUBLISH_TOKEN: 'fixture', CI_PIPELINE_SOURCE: 'push',
    CI_DEFAULT_BRANCH: 'master', CI_COMMIT_BRANCH: 'master', CI_COMMIT_REF_PROTECTED: 'true' }
  const result = spawnSync(process.execPath, [resolve(scripts, 'release-npm.mjs')], { cwd: root, env, encoding: 'utf8' })
  return { result, calls: await readFile(log, 'utf8').catch(() => '') }
}

test('an existing Core version is skipped without npm writes', async t => {
  const { result, calls } = await fixture(t, true)
  assert.equal(result.status, 0, result.stderr)
  assert.match(result.stdout, /already exists; no new version to publish/)
  assert.equal(calls, '')
})

test('a new Core version is published once and its registry integrity verified', async t => {
  const { result, calls } = await fixture(t, false)
  assert.equal(result.status, 0, result.stderr)
  assert.match(result.stdout, /Published and verified/)
  assert.deepEqual(calls.trim().split('\n'), ['pack', 'whoami', 'publish'])
})

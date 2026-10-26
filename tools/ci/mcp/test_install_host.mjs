// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { chmod, copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import test from 'node:test'

const installer = fileURLToPath(new URL('./install_host.mjs', import.meta.url))
const realNpm = resolve(dirname(process.execPath), 'npm')

async function fixture(t) {
  const root = await mkdtemp(join(tmpdir(), 'host install fixture '))
  t.after(() => rm(root, { recursive: true, force: true }))
  const env = { ...process.env, npm_config_cache: join(root, 'cache'), npm_config_offline: 'true',
    TMPDIR: root, TMP: root, TEMP: root }
  const npm = (args, cwd) => {
    const result = spawnSync(process.execPath, [realNpm, ...args], { cwd, env, encoding: 'utf8' })
    assert.equal(result.status, 0, result.stderr)
    return result.stdout
  }
  const core = join(root, 'host/device-core'), cli = join(root, 'host/device-cli')
  const helper = join(root, 'helper'), artifacts = join(root, 'artifacts')
  for (const directory of [core, cli, helper, artifacts, join(root, 'tools/ci/mcp')]) await mkdir(directory, { recursive: true })
  await copyFile(installer, join(root, 'tools/ci/mcp/install_host.mjs'))
  await writeFile(join(core, 'package.json'), JSON.stringify({ name: '@esp-vision/device-core', version: '0.5.1', files: ['index.js'] }))
  await writeFile(join(core, 'index.js'), 'export const resetLifecycle = "published"\n')
  const [oldCore] = JSON.parse(npm(['pack', '--json', '--ignore-scripts', '--pack-destination', artifacts], core))
  await writeFile(join(core, 'index.js'), 'export const resetLifecycle = "fixed"\n')
  await writeFile(join(helper, 'package.json'), JSON.stringify({ name: 'host-lock-fixture', version: '1.0.0', files: ['index.js'] }))
  await writeFile(join(helper, 'index.js'), 'export const unchanged = true\n')
  const [other] = JSON.parse(npm(['pack', '--json', '--ignore-scripts', '--pack-destination', artifacts], helper))
  npm(['cache', 'add', join(artifacts, other.filename)], root)
  const manifest = { name: '@esp-vision/device-cli', version: '0.1.0', devDependencies: {
    '@esp-vision/device-core': '0.5.1', 'host-lock-fixture': '1.0.0',
  } }
  const lock = { name: manifest.name, version: manifest.version, lockfileVersion: 3, requires: true, packages: {
    '': { name: manifest.name, version: manifest.version, devDependencies: manifest.devDependencies },
    'node_modules/@esp-vision/device-core': { version: oldCore.version, dev: true, integrity: oldCore.integrity,
      resolved: 'https://registry.npmjs.org/@esp-vision/device-core/-/device-core-0.5.1.tgz' },
    'node_modules/host-lock-fixture': { version: other.version, dev: true, integrity: other.integrity,
      resolved: 'https://registry.npmjs.org/host-lock-fixture/-/host-lock-fixture-1.0.0.tgz' },
  } }
  const tracked = JSON.stringify(lock, null, 2) + '\n'
  await writeFile(join(cli, 'package.json'), JSON.stringify(manifest, null, 2) + '\n')
  await writeFile(join(cli, 'package-lock.json'), tracked)
  return { root, cli, env, tracked, run() {
    return spawnSync(process.execPath, ['tools/ci/mcp/install_host.mjs', '--output', artifacts], {
      cwd: root, env, encoding: 'utf8', maxBuffer: 2 * 1024 * 1024,
    })
  } }
}

async function injectNpmFault(f, action) {
  const bin = join(f.root, 'bin')
  await mkdir(bin)
  const wrapper = join(bin, 'npm')
  await writeFile(wrapper, `#!/usr/bin/env node
const { spawnSync } = require('node:child_process')
const fs = require('node:fs')
const path = require('node:path')
const args = process.argv.slice(2)
const result = spawnSync(process.execPath, [${JSON.stringify(realNpm)}, ...args], { stdio: 'inherit' })
if (result.status) process.exit(result.status)
if (path.basename(process.cwd()) === 'device-cli') {
${action}
}
`)
  await chmod(wrapper, 0o755)
  f.env.PATH = `${bin}:${f.env.PATH}`
}

test('MR installs changed same-version Core bytes while retaining the registry lock and unrelated closure', async t => {
  const f = await fixture(t)
  const result = f.run()
  assert.equal(result.status, 0, result.stderr)
  assert.equal(await readFile(join(f.cli, 'package-lock.json'), 'utf8'), f.tracked)
  assert.equal(await readFile(join(f.cli, 'node_modules/@esp-vision/device-core/index.js'), 'utf8'),
    'export const resetLifecycle = "fixed"\n')
  assert.equal(await readFile(join(f.cli, 'node_modules/host-lock-fixture/index.js'), 'utf8'),
    'export const unchanged = true\n')
})

test('MR staging fails if npm changes an unrelated locked dependency', async t => {
  const f = await fixture(t)
  await injectNpmFault(f, `if (args.includes('--package-lock-only')) {
  const lock = JSON.parse(fs.readFileSync('package-lock.json'))
  lock.packages['node_modules/host-lock-fixture'].integrity = 'sha512-tampered'
  fs.writeFileSync('package-lock.json', JSON.stringify(lock))
}`)
  const result = f.run()
  assert.notEqual(result.status, 0)
  assert.match(result.stderr, /Unrelated locked dependency changed: node_modules\/host-lock-fixture/)
})

test('MR staging rejects installed Core bytes that differ from the verified artifact source', async t => {
  const f = await fixture(t)
  await injectNpmFault(f, `if (args[0] === 'ci') fs.writeFileSync('node_modules/@esp-vision/device-core/index.js', 'tampered')`)
  const result = f.run()
  assert.notEqual(result.status, 0)
  assert.match(result.stderr, /Installed Core differs from the staged source: index.js/)
})

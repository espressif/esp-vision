// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { copyFile, mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { test } from 'node:test'

const script = fileURLToPath(new URL('./package_plugin_release.mjs', import.meta.url))
async function check(t, pins, registryCliCore, knowledgePins = pins) {
  const root = await mkdtemp(join(tmpdir(), 'plugin-release-pins-'))
  t.after(() => rm(root, { recursive: true, force: true }))
  const helper = join(root, 'tools/ci/mcp/package_plugin_release.mjs')
  await mkdir(dirname(helper), { recursive: true })
  await mkdir(join(root, 'host/esp-vision-plugin'), { recursive: true })
  await mkdir(join(root, 'host/knowledge-mcp'), { recursive: true })
  await copyFile(script, helper)
  await writeFile(join(root, 'host/esp-vision-plugin/package.json'), JSON.stringify({ espVision: pins }))
  await writeFile(join(root, 'host/knowledge-mcp/package.json'), JSON.stringify({ espVision: knowledgePins }))
  const loader = join(root, 'registry.mjs')
  await writeFile(loader, `globalThis.fetch=async url=>{const part=decodeURIComponent(url.pathname).split('/');const cli=part[2]==='device-cli';return {status:200,json:async()=>({name:'@esp-vision/'+part[2],version:part[3],dist:{integrity:'sha512-test'},devDependencies:cli?{'@esp-vision/device-core':${JSON.stringify(registryCliCore)}}:{}})}}`)
  return spawnSync(process.execPath, [helper, '--output', join(root, 'out')], {
    env: { ...process.env, TMPDIR: root, TMP: root, TEMP: root, NODE_OPTIONS: `--import ${pathToFileURL(loader).href}` }, encoding: 'utf8',
  })
}

test('release rejects ranges instead of exact CLI and Core versions', async t => {
  const result = await check(t, { deviceCore: '^0.5.1', deviceCli: '0.1.0' }, '0.5.1')
  assert.notEqual(result.status, 0)
  assert.match(result.stderr, /Pinned versions must be exact/)
})

test('release rejects a published CLI declaring a different Core', async t => {
  const result = await check(t, { deviceCore: '0.5.1', deviceCli: '0.1.0' }, '0.4.2')
  assert.notEqual(result.status, 0)
  assert.match(result.stderr, /Published CLI must declare the pinned Core version/)
})

test('release rejects different Plugin and Knowledge compatibility pins', async t => {
  const result = await check(t, { deviceCore: '0.5.1', deviceCli: '0.1.0' }, '0.5.1', { deviceCore: '0.4.2', deviceCli: '0.1.0' })
  assert.notEqual(result.status, 0)
  assert.match(result.stderr, /matching release pins/)
})

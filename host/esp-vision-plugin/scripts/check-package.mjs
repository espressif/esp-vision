// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { access, readFile, readdir, stat } from 'node:fs/promises'
import { constants } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const pluginRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const plugin = JSON.parse(await readFile(resolve(pluginRoot, 'plugin.json'), 'utf8'))
const npmPackage = JSON.parse(await readFile(resolve(pluginRoot, 'package.json'), 'utf8'))
assert.equal(npmPackage.name, '@esp-vision/plugin')
assert.equal(npmPackage.version, plugin.version)
assert.deepEqual(npmPackage.dependencies ?? {}, {})
assert.ok(npmPackage.files.includes('cli'))
assert.ok(npmPackage.files.includes('skills'))
assert.ok(npmPackage.files.includes('assets'))
assert.ok(npmPackage.files.includes('workbuddy'))
assert.ok(!npmPackage.files.includes('server'))
const mcp = JSON.parse(await readFile(resolve(pluginRoot, 'mcp.json'), 'utf8'))
const skill = await readFile(resolve(pluginRoot, 'skills/esp-vision-development/SKILL.md'), 'utf8')

assert.equal(plugin.$schema, 'https://agent-plugins.org/schemas/1.0.0/plugin.schema.json')
assert.match(plugin.name, /^(?!.*(?:--|\.\.))[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?$/)
assert.match(plugin.version, /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/)
assert.equal(plugin.extensions?.['com.openai']?.requires_local_executor, true)
const officialLogo = './assets/esp-vision-logo.png'
assert.equal(plugin.extensions?.['com.openai']?.interface?.logo, officialLogo)
assert.equal(plugin.extensions?.['com.openai']?.interface?.composerIcon, officialLogo)
const logo = await readFile(resolve(pluginRoot, officialLogo))
assert.equal(createHash('sha256').update(logo).digest('hex'),
  '9d9aab9c27f0cf9465174467698d5ffecd8b344cfd1d460f01409c6f43611e11',
  'the official logo must be preserved byte-for-byte')
assert.equal(logo.subarray(0, 8).toString('hex'), '89504e470d0a1a0a')
assert.equal(logo.readUInt32BE(16), 302)
assert.equal(logo.readUInt32BE(20), 302)
await access(resolve(pluginRoot, 'assets/PROVENANCE.md'))
assert.equal(mcp.$schema, 'https://agent-plugins.org/schemas/1.0.0/mcp.schema.json')
assert.deepEqual(Object.keys(mcp.mcpServers).sort(), ['esp-vision-knowledge'])
assert.deepEqual(mcp.mcpServers['esp-vision-knowledge'], {
  type: 'streamable-http', url: 'https://mcp.vision.espressif.com/mcp',
})
assert.match(skill, /^---\nname: esp-vision-development\ndescription: .+\n---\n/m)
assert.match(skill, /esp-vision-knowledge/)
assert.match(skill, /cli\/device-cli\.mjs/)
assert.match(skill, /uncertain/i)
assert.match(skill, /do not repeat/i)
assert.match(skill, /@esp-vision\/device-cli/)
await access(resolve(pluginRoot, 'skills/esp-vision-development/references/application-workflow.md'))
assert.doesNotMatch(skill, /device-mcp\.mjs|core-cli\.mjs|npx .*device-cli/)
assert.deepEqual(await readdir(resolve(pluginRoot, 'skills')), ['esp-vision-development'])

const claude = JSON.parse(await readFile(resolve(pluginRoot, '.claude-plugin/plugin.json'), 'utf8'))
assert.equal(claude.name, plugin.name)
assert.equal(claude.version, plugin.version)
assert.equal(claude.icon, officialLogo)
const claudeMcp = JSON.parse(await readFile(resolve(pluginRoot, '.mcp.json'), 'utf8'))
assert.deepEqual(Object.keys(claudeMcp.mcpServers).sort(), Object.keys(mcp.mcpServers).sort())
assert.deepEqual(claudeMcp.mcpServers['esp-vision-knowledge'], {
  type: 'http', url: mcp.mcpServers['esp-vision-knowledge'].url,
})
const workbuddyMcp = JSON.parse(await readFile(resolve(pluginRoot, 'workbuddy/mcp.json'), 'utf8'))
assert.deepEqual(workbuddyMcp.mcpServers, {
  'esp-vision-knowledge': {
    type: 'streamableHttp', url: mcp.mcpServers['esp-vision-knowledge'].url, timeout: 30000,
  },
})
await access(resolve(pluginRoot, 'workbuddy/README.md'))
for (const catalog of ['.agents/plugins/marketplace.json', '.claude-plugin/marketplace.json']) {
  const data = JSON.parse(await readFile(resolve(pluginRoot, catalog), 'utf8'))
  assert.equal(data.name, 'esp-vision')
  assert.equal(data.plugins.length, 1)
  assert.equal(data.plugins[0].name, plugin.name)
  assert.equal(typeof data.plugins[0].source === 'string' ? data.plugins[0].source : data.plugins[0].source.path, './')
}

const cli = resolve(pluginRoot, 'cli')
for (const name of ['device-cli.mjs', 'node_serial.py', 'node_rom.py', 'licenses']) await access(resolve(cli, name))
// Common npm dependencies use lowercase license filenames. Preserve their
// actual notices rather than treating package metadata as the license text.
for (const name of ['ansi-regex-license', 'multicast-dns-LICENSE']) {
  await access(resolve(cli, 'licenses', name))
}
await access(resolve(pluginRoot, 'LICENSE'))
await access(resolve(pluginRoot, 'DEVICE_CORE_NOTICE.md'))
await access(resolve(pluginRoot, 'THIRD_PARTY_NOTICES.md'))
const notices = await readFile(resolve(pluginRoot, 'THIRD_PARTY_NOTICES.md'), 'utf8')
assert.doesNotMatch(notices, /\]\(cli\/licenses\//, 'generated bundle paths must not be broken source-tree Markdown links')
const mode = (await stat(resolve(cli, 'device-cli.mjs'))).mode
assert.ok(mode & constants.S_IXUSR)
const bundle = await readFile(resolve(cli, 'device-cli.mjs'), 'utf8')
assert.ok(bundle.startsWith('#!/usr/bin/env node'))
assert.doesNotMatch(bundle, /^\s*(?:import|export)\s+(?:[^'";]+\s+from\s+)?['"](?!node:)[^'"]+['"]/m,
  'bundled CLI entrypoint must not have unresolved package imports')
await assert.rejects(access(resolve(pluginRoot, 'server/device-mcp.mjs')))
await assert.rejects(access(resolve(cli, 'core-cli.mjs')))

console.log('ESP-VISION plugin manifests, capability Skill and bundled standalone CLI are valid.')

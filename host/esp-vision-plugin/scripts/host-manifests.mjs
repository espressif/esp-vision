// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { readFile, writeFile } from 'node:fs/promises'
import { resolve } from 'node:path'

// These tracked compatibility files are generated views, never independent inputs.
export async function syncHostManifests(root, { check = false } = {}) {
  const plugin = JSON.parse(await readFile(resolve(root, 'plugin.json'), 'utf8'))
  const version = JSON.parse(await readFile(resolve(root, 'package.json'), 'utf8')).version
  if (check) assert.equal(plugin.version, version, 'plugin.json version is generated from package.json')
  else {
    plugin.version = version
    await writeFile(resolve(root, 'plugin.json'), JSON.stringify(plugin, null, 2) + '\n')
  }
  const mcp = JSON.parse(await readFile(resolve(root, 'mcp.json'), 'utf8'))
  const { name, description, author, homepage, repository, license, keywords } = plugin
  const knowledgeUrl = mcp.mcpServers['esp-vision-knowledge'].url
  const files = new Map([
    ['.agents/plugins/marketplace.json', { name, interface: { displayName: 'ESP-VISION' }, plugins: [{ name, source: { source: 'local', path: './' }, policy: { installation: 'AVAILABLE', authentication: 'ON_INSTALL' }, category: 'Productivity' }] }],
    ['.claude-plugin/marketplace.json', { name, owner: { name: author.name, url: author.url }, plugins: [{ name, source: './', description }], description }],
    ['.claude-plugin/plugin.json', { name, version, description, icon: plugin.extensions['com.openai'].interface.logo, author, homepage, repository, license, keywords }],
    ['.mcp.json', { mcpServers: { 'esp-vision-knowledge': { type: 'http', url: knowledgeUrl } } }],
    ['workbuddy/mcp.json', { mcpServers: { 'esp-vision-knowledge': { type: 'streamableHttp', url: knowledgeUrl, timeout: 30000 } } }],
  ])
  for (const [name, data] of files) {
    const path = resolve(root, name)
    if (check) assert.deepEqual(JSON.parse(await readFile(path, 'utf8')), data, `${name} is generated; rebuild from plugin.json and mcp.json`)
    else await writeFile(path, JSON.stringify(data, null, 2) + '\n')
  }
}

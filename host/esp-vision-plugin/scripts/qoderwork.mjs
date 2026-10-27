// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createExport, deviceReference, json, readTemplate, renderListing, sharedMetadata, validatePublisher, skillName, sourceCommit } from './export-common.mjs'

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const templateRoot = resolve(packageRoot, 'qoderwork')

export function validateQoderManifest(manifest) {
  assert.equal(manifest.name, 'esp-vision')
  assert.equal(manifest.displayName, 'ESP-Vision 嵌入式 AI 视觉开发')
  assert.match(manifest.version, /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/)
  assert.deepEqual(manifest.skills, [`./skills/${skillName}`])
  assert.equal(manifest.mcpServers, './.mcp.json')
  validatePublisher(manifest.author)
  assert.ok(manifest.keywords.includes('ESP32'))
  assert.equal(manifest.license, 'Apache-2.0')
}

export function adaptQoderSkill(skill, version) {
  const frontmatter = /^---\nname: esp-vision-development\ndescription: [^\n]+\n---/.exec(skill)
  assert.ok(frontmatter, 'Common Skill frontmatter changed; review QoderWork adaptation')
  const body = skill.slice(frontmatter[0].length)
  return `---\nname: ${skillName}\ndescription: ${JSON.stringify('帮助用户基于 ESP32 开发 ESP-VISION 视觉应用，查询官方固件和 API 知识，并使用随包本地 CLI 检查设备、部署 MicroPython 脚本和验收结果。适用于摄像头、图像处理和设备开发任务。')}\nversion: ${version}\n---${body}`
}

/** Offline candidate builder. Does not install, contact devices, or submit a marketplace release. */
export async function buildQoderArchive({ sourceRoot = packageRoot, outputDirectory, ciEnvironment = process.env } = {}) {
  const bundle = await createExport({ sourceRoot, packageRoot, outputDirectory, rootName: 'esp-vision-qoderwork' })
  const { source, original, knowledgeUrl, add } = bundle
  const manifest = { ...sharedMetadata(original, JSON.parse(await readTemplate(templateRoot, 'plugin.json'))),
    skills: [`./skills/${skillName}`], mcpServers: './.mcp.json' }
  validateQoderManifest(manifest)
  add('.qoder-plugin/plugin.json', json(manifest))
  add('.mcp.json', json({ mcpServers: { 'esp-vision-knowledge': {
    type: 'streamable-http', url: '{{USER_CONFIG}}',
    _setup: { configUrl: original.homepage, guide: `填写 ESP-VISION 官方文档 MCP 地址：${knowledgeUrl}；该公开知识服务不需要设备地址或凭证。`, required: true },
  } } }))
  add(`skills/${skillName}/SKILL.md`, adaptQoderSkill(await readFile(resolve(source, `skills/${skillName}/SKILL.md`), 'utf8'), manifest.version))
  const reference = await readFile(resolve(source, `skills/${skillName}/references/application-workflow.md`), 'utf8')
  add(`skills/${skillName}/references/application-workflow.md`, deviceReference(reference, '# Installation and application verification\n\nLocate the installed QoderWork plugin root from its actual Skill installation path. Verify cli/device-cli.mjs and the bundled licenses exist. Preserve uncertain or modified installations before using the host native uninstall/reinstall flow. Do not edit an unrecognized directory or infer successful activation from a ZIP build. Verify the Skill and qoder.md role instructions are active and the official Knowledge MCP connection works. A matching version does not prove matching payload bytes.\n\nThe following device acceptance guidance applies after installation and explicit device selection. Finish directory operations and uploads before starting a continuous application.'))
  for (const name of ['README.md', 'CONNECTORS.md', 'qoder.md']) {
    add(name, renderListing(await readTemplate(templateRoot, name), manifest))
  }
  bundle.provenance('qoderwork', manifest.version, sourceCommit(source, ciEnvironment, 'QoderWork'))
  return { ...(await bundle.finish(`esp-vision-qoderwork-${manifest.version}.zip`)), version: manifest.version }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  assert.equal(process.argv.length, 4, 'Usage: node scripts/qoderwork.mjs --output-dir <directory>')
  assert.equal(process.argv[2], '--output-dir')
  process.stdout.write(json(await buildQoderArchive({ outputDirectory: process.argv[3] })))
}

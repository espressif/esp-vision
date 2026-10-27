// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createExport, deviceReference, json, readTemplate, renderListing, sharedMetadata, skillName, sourceCommit as readSourceCommit } from './export-common.mjs'

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const templateRoot = resolve(packageRoot, 'cursor')
const template = name => readTemplate(templateRoot, name)

export function adaptCursorSkill(skill) {
  assert.match(skill, /^---\nname: esp-vision-development\ndescription: [^\n]+\n---/)
  return skill
}

/** Export a complete Cursor Plugin payload. Does not install, connect, or publish. */
export async function buildCursorArchive({ sourceRoot = packageRoot, outputDirectory, ciEnvironment = process.env } = {}) {
  const bundle = await createExport({ sourceRoot, packageRoot, outputDirectory, rootName: 'esp-vision' })
  const { source, original, knowledgeUrl, add } = bundle
  const manifest = { ...sharedMetadata(original, JSON.parse(await template('plugin.json'))),
    skills: [`./skills/${skillName}`], mcpServers: './mcp.json', logo: original.extensions['com.openai'].interface.logo.replace(/^\.\//, '') }
  assert.deepEqual(manifest.skills, [`./skills/${skillName}`])
  assert.equal(manifest.mcpServers, './mcp.json')
  const sourceCommit = readSourceCommit(source, ciEnvironment, 'Cursor')
  add('.cursor-plugin/plugin.json', json(manifest))
  add('mcp.json', json({ mcpServers: { 'esp-vision-knowledge': { url: knowledgeUrl } } }))
  add(`skills/${skillName}/SKILL.md`, adaptCursorSkill(await readFile(resolve(source, `skills/${skillName}/SKILL.md`), 'utf8')))
  const reference = await readFile(resolve(source, `skills/${skillName}/references/application-workflow.md`), 'utf8')
  add(`skills/${skillName}/references/application-workflow.md`, deviceReference(reference, '# Cursor installation and application verification\n\nCopy the complete esp-vision directory into ~/.cursor/plugins/local, preserving existing unknown or modified copies. Reload Cursor and verify the Skill and Knowledge MCP in Customize. Respect enterprise local-import policy; do not bypass a denial. An installed marketplace plugin with the same name takes precedence over this local copy. This archive build does not establish activation or approval. Locate the CLI from the installed Skill before using it; a version alone is not proof of matching payloads.'))
  add('README.md', renderListing(await template('README.md'), manifest))
  bundle.provenance('cursor', manifest.version, sourceCommit)
  return { ...(await bundle.finish(`esp-vision-cursor-${manifest.version}.zip`)), version: manifest.version, sourceCommit }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  assert.equal(process.argv.length, 4, 'Usage: node scripts/cursor.mjs --output-dir DIRECTORY')
  assert.equal(process.argv[2], '--output-dir')
  process.stdout.write(json(await buildCursorArchive({ outputDirectory: process.argv[3] })))
}

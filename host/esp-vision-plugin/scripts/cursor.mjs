// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { lstat, mkdir, readFile, realpath, writeFile } from 'node:fs/promises'
import { dirname, isAbsolute, relative, resolve, sep } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createZip, validatePackage } from '../src/installer.mjs'

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const templateRoot = resolve(packageRoot, 'cursor')
const skillName = 'esp-vision-development'
const rootName = 'esp-vision'
const json = value => `${JSON.stringify(value, null, 2)}\n`
const digest = bytes => createHash('sha256').update(bytes).digest('hex')
const inside = (parent, path) => {
  const rel = relative(parent, path)
  return rel === '' || (!isAbsolute(rel) && rel !== '..' && !rel.startsWith(`..${sep}`))
}
async function template(name) {
  const path = resolve(templateRoot, name)
  const info = await lstat(path)
  assert.ok(info.isFile() && !info.isSymbolicLink(), `Invalid Cursor template: ${name}`)
  return readFile(path, 'utf8')
}

export function adaptCursorSkill(skill) {
  assert.match(skill, /^---\nname: esp-vision-development\ndescription: [^\n]+\n---/)
  const replacements = new Map([
    ['Knowledge MCP provides documentation;', 'Knowledge MCP provides read-only official documentation; device operations use the bundled local CLI. Match API and hardware claims to the actual board and firmware. A built archive does not prove Cursor loaded it, connected the Knowledge MCP, or approved a marketplace listing. See the application workflow reference below for installation and device acceptance.'],
    ['Invoke the installed plugin copy with Node', 'Invoke Node with the absolute path to this installed plugin’s `cli/device-cli.mjs`. Locate the current Skill installation from Cursor Customize or the installed Skill file; the plugin root is two directories above `skills/esp-vision-development`. Verify the CLI exists and set the shell variable CLI to that absolute file path, quoting paths with spaces. Cursor documents plugin-root substitution in MCP configuration, not a guaranteed environment variable in arbitrary terminal commands. Do not guess the path from the user project, use a global CLI or npx, or add a Device MCP wrapper. The standalone `@esp-vision/device-cli` remains available outside the plugin.'],
  ])
  let paragraphs = skill.split('\n\n')
  for (const [prefix, replacement] of replacements) {
    assert.equal(paragraphs.filter(value => value.startsWith(prefix)).length, 1, `Common Skill changed: ${prefix}`)
    paragraphs = paragraphs.map(value => value.startsWith(prefix) ? replacement : value)
  }
  const adapted = paragraphs.join('\n\n')
    .replace('node <plugin-root>/cli/device-cli.mjs --help', 'node "${CLI:?Set CLI to the verified installed bundle path}" --help')
  assert.doesNotMatch(adapted, /CLAUDE_PLUGIN_ROOT|CODEBUDDY_PLUGIN_ROOT|WorkBuddy|<plugin-root>/)
  return adapted
}

/** Export a complete Cursor Plugin payload. Does not install, connect, or publish. */
export async function buildCursorArchive({ sourceRoot = packageRoot, outputDirectory, ciEnvironment = process.env } = {}) {
  assert.ok(outputDirectory, 'Pass --output-dir outside the plugin source')
  const source = await realpath(resolve(sourceRoot))
  await mkdir(resolve(outputDirectory), { recursive: true })
  const output = await realpath(resolve(outputDirectory))
  assert.ok(!inside(source, output) && !inside(packageRoot, output), 'Output must be outside the plugin source')
  const info = await validatePackage(source)
  const original = JSON.parse(await readFile(resolve(source, 'plugin.json'), 'utf8'))
  const manifest = JSON.parse(await template('plugin.json'))
  for (const key of ['name', 'version', 'homepage', 'repository', 'license']) assert.equal(manifest[key], original[key], `Cursor ${key} must match the shared plugin`)
  assert.deepEqual(manifest.skills, [`./skills/${skillName}`])
  assert.equal(manifest.mcpServers, './mcp.json')
  assert.equal(manifest.logo, 'assets/esp-vision-logo.png')
  assert.equal(manifest.author.name, original.author.name)
  const inCI = ciEnvironment.CI_COMMIT_SHA !== undefined || ciEnvironment.CI
  const sourceCommit = inCI ? ciEnvironment.CI_COMMIT_SHA ?? ''
    : execFileSync('git', ['-C', source, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim()
  assert.ok(sourceCommit.length === 40 && /^[a-f0-9]{40}$/.test(sourceCommit), 'Cursor source commit must be a complete lowercase 40-character Git SHA')
  const files = new Map()
  const add = (path, contents, mode = 0o644) => {
    assert.ok(!isAbsolute(path) && !path.includes('\\') && !path.split('/').some(part => !part || part === '..' || part === '.'))
    assert.ok(!files.has(path), `Duplicate Cursor path: ${path}`)
    files.set(path, { name: `${rootName}/${path}`, contents: Buffer.from(contents), mode })
  }
  for (const file of info.files.filter(file => /^(cli|assets)\//.test(file.path))) {
    add(file.path, await readFile(resolve(source, file.path)), file.mode & 0o111 ? 0o755 : 0o644)
  }
  for (const path of ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'DEVICE_CORE_NOTICE.md']) add(path, await readFile(resolve(source, path)))
  const sharedMcp = JSON.parse(await readFile(resolve(source, 'mcp.json'), 'utf8'))
  assert.deepEqual(sharedMcp.mcpServers, { 'esp-vision-knowledge': { type: 'streamable-http', url: 'https://mcp.vision.espressif.com/mcp' } })
  add('.cursor-plugin/plugin.json', json(manifest))
  // Cursor Plugin URL discovery needs no variables, credentials or local device server.
  add('mcp.json', json({ mcpServers: { 'esp-vision-knowledge': { url: sharedMcp.mcpServers['esp-vision-knowledge'].url } } }))
  add(`skills/${skillName}/SKILL.md`, adaptCursorSkill(await readFile(resolve(source, `skills/${skillName}/SKILL.md`), 'utf8')))
  const reference = await readFile(resolve(source, `skills/${skillName}/references/application-workflow.md`), 'utf8')
  const sections = reference.split('## Hardware API and capture context')
  assert.equal(sections.length, 2, 'Common application workflow changed')
  add(`skills/${skillName}/references/application-workflow.md`, '# Cursor installation and application verification\n\nCopy the complete esp-vision directory into ~/.cursor/plugins/local, preserving existing unknown or modified copies. Reload Cursor and verify the Skill and Knowledge MCP in Customize. Respect enterprise local-import policy; do not bypass a denial. An installed marketplace plugin with the same name takes precedence over this local copy. This archive build does not establish activation or approval. Locate the CLI from the installed Skill before using it; a version alone is not proof of matching payloads.\n\n## Hardware API and capture context' + sections[1]
    .replace('CLI=/absolute/installed/plugin/cli/device-cli.mjs', ': "${CLI:?Set CLI to the verified installed bundle path}"')
    .replace('PORT=/dev/selected-device', ': "${PORT:?Set PORT to the explicitly selected serial device}"'))
  add('README.md', (await template('README.md')).replaceAll('{{VERSION}}', manifest.version))
  const payload = [...files].map(([path, entry]) => ({ path, sha256: digest(entry.contents) }))
  add('BUILD-PROVENANCE.json', json({ schemaVersion: 1, target: 'cursor', version: manifest.version, sourceVersion: info.version, sourceCommit, files: payload }))
  const archive = createZip([...files.values()].sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0))
  const name = `esp-vision-cursor-${manifest.version}.zip`
  for (const nameToCheck of [name, `${name}.sha256`]) {
    await assert.rejects(lstat(resolve(output, nameToCheck)), error => error.code === 'ENOENT', `Refusing to overwrite Cursor artifact: ${nameToCheck}`)
  }
  const sha256 = digest(archive)
  const archivePath = resolve(output, name)
  await writeFile(archivePath, archive, { flag: 'wx' })
  await writeFile(`${archivePath}.sha256`, `${sha256}  ${name}\n`, { flag: 'wx' })
  return { archivePath, sha256, version: manifest.version, sourceCommit }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  assert.equal(process.argv.length, 4, 'Usage: node scripts/cursor.mjs --output-dir DIRECTORY')
  assert.equal(process.argv[2], '--output-dir')
  process.stdout.write(json(await buildCursorArchive({ outputDirectory: process.argv[3] })))
}

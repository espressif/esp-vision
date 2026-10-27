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
const templateRoot = resolve(packageRoot, 'qoderwork')
const skillName = 'esp-vision-development'
const rootName = 'esp-vision-qoderwork'
const json = value => `${JSON.stringify(value, null, 2)}\n`
const hash = bytes => createHash('sha256').update(bytes).digest('hex')
const inside = (parent, path) => {
  const rel = relative(parent, path)
  return rel === '' || (!isAbsolute(rel) && rel !== '..' && !rel.startsWith(`..${sep}`))
}

export function validateQoderManifest(manifest) {
  assert.equal(manifest.name, 'esp-vision')
  assert.equal(manifest.displayName, 'ESP-Vision 嵌入式 AI 视觉开发')
  assert.match(manifest.version, /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/)
  assert.deepEqual(manifest.skills, [`./skills/${skillName}`])
  assert.equal(manifest.mcpServers, './.mcp.json')
  assert.deepEqual(manifest.author, { name: 'Espressif Systems (Shanghai) CO LTD', email: 'libo@espressif.com' })
  assert.ok(manifest.keywords.includes('ESP32'))
  assert.equal(manifest.license, 'Apache-2.0')
}

export function adaptQoderSkill(skill, version) {
  const frontmatter = /^---\nname: esp-vision-development\ndescription: [^\n]+\n---/.exec(skill)
  assert.ok(frontmatter, 'Common Skill frontmatter changed; review QoderWork adaptation')
  const paragraphs = skill.slice(frontmatter[0].length).split('\n\n')
  const prefix = 'Invoke the installed plugin copy with Node'
  assert.equal(paragraphs.filter(value => value.startsWith(prefix)).length, 1)
  let body = paragraphs.map(value => value.startsWith(prefix)
    ? 'Invoke Node with the absolute path to `cli/device-cli.mjs` in this installed QoderWork plugin. Locate the installed Skill using the host installation path; resolve the plugin root two directories above the `esp-vision-development` directory and verify the CLI exists. Do not infer the path from the user project. The QoderWork public guide does not define a plugin-root environment variable, so do not invent one or use another host’s variable. Set the shell variable CLI to that verified absolute CLI file path before using the guarded commands below. Quote absolute paths, including on Windows. If resources are absent, report installation failure. Do not use a global CLI, npx, or Device MCP wrapper.'
    : value).join('\n\n')
  body = body.replace(/Knowledge MCP provides documentation; device operations intentionally use the local CLI\.[\s\S]*?see the application workflow reference below for support checks\./, 'Knowledge MCP provides documentation; device operations use the local CLI. Check official knowledge against the actual board and firmware before claiming support. QoderWork installation and marketplace approval require actual host evidence; a local ZIP build is insufficient. See the application workflow reference below for device acceptance criteria.')
  body = body.replace('node <plugin-root>/cli/device-cli.mjs --help', 'node "${CLI:?Set CLI to the verified installed bundle path}" --help')
  assert.doesNotMatch(body, /CLAUDE_PLUGIN_ROOT|CODEBUDDY_PLUGIN_ROOT|`PLUGIN_ROOT`/)
  return `---\nname: ${skillName}\ndescription: ${JSON.stringify('帮助用户基于 ESP32 开发 ESP-VISION 视觉应用，查询官方固件和 API 知识，并使用随包本地 CLI 检查设备、部署 MicroPython 脚本和验收结果。适用于摄像头、图像处理和设备开发任务。')}\nversion: ${version}\n---${body}`
}

/** Offline candidate builder. Does not install, contact devices, or submit a marketplace release. */
export async function buildQoderArchive({ sourceRoot = packageRoot, outputDirectory } = {}) {
  assert.ok(outputDirectory, 'Pass --output-dir outside the plugin source')
  const source = await realpath(resolve(sourceRoot))
  await mkdir(resolve(outputDirectory), { recursive: true })
  const output = await realpath(resolve(outputDirectory))
  assert.ok(!inside(source, output) && !inside(packageRoot, output), 'Output must be outside the plugin source')
  const info = await validatePackage(source)
  const manifest = JSON.parse(await readFile(resolve(templateRoot, 'plugin.json'), 'utf8'))
  validateQoderManifest(manifest)
  const entries = []
  const add = (path, contents, mode = 0o644) => {
    assert.ok(!isAbsolute(path) && !path.includes('\\') && !path.split('/').some(part => !part || part === '..' || part === '.'))
    entries.push({ name: `${rootName}/${path}`, contents: Buffer.from(contents), mode })
  }
  for (const file of info.files.filter(file => /^(cli|assets)\//.test(file.path))) {
    add(file.path, await readFile(resolve(source, file.path)), file.mode & 0o111 ? 0o755 : 0o644)
  }
  for (const path of ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'DEVICE_CORE_NOTICE.md']) add(path, await readFile(resolve(source, path)))
  add('.qoder-plugin/plugin.json', json(manifest))
  add('.mcp.json', json({ mcpServers: { 'esp-vision-knowledge': {
    type: 'streamable-http', url: '{{USER_CONFIG}}',
    _setup: { configUrl: 'https://vision.espressif.com/', guide: '填写 ESP-VISION 官方文档 MCP 地址：https://mcp.vision.espressif.com/mcp；该公开知识服务不需要设备地址或凭证。', required: true },
  } } }))
  add(`skills/${skillName}/SKILL.md`, adaptQoderSkill(await readFile(resolve(source, `skills/${skillName}/SKILL.md`), 'utf8'), manifest.version))
  const reference = await readFile(resolve(source, `skills/${skillName}/references/application-workflow.md`), 'utf8')
  const sections = reference.split('## Hardware API and capture context')
  assert.equal(sections.length, 2, 'Common hardware acceptance reference changed; review adaptation')
  add(`skills/${skillName}/references/application-workflow.md`, '# Installation and application verification\n\nLocate the installed QoderWork plugin root from its actual Skill installation path. Verify cli/device-cli.mjs and the bundled licenses exist. Preserve uncertain or modified installations before using the host native uninstall/reinstall flow. Do not edit an unrecognized directory or infer successful activation from a ZIP build. Verify the Skill and qoder.md role instructions are active and the official Knowledge MCP connection works. A matching version does not prove matching payload bytes.\n\nThe following device acceptance guidance applies after installation and explicit device selection. Finish directory operations and uploads before starting a continuous application.\n\n## Hardware API and capture context' + sections[1].replace('CLI=/absolute/installed/plugin/cli/device-cli.mjs', ': "${CLI:?Set CLI to the verified installed bundle path}"').replace('PORT=/dev/selected-device', ': "${PORT:?Set PORT to the explicitly selected serial device}"'))
  for (const name of ['README.md', 'CONNECTORS.md', 'qoder.md']) {
    const path = resolve(templateRoot, name)
    assert.ok((await lstat(path)).isFile() && !(await lstat(path)).isSymbolicLink())
    add(name, (await readFile(path, 'utf8')).replaceAll('{{VERSION}}', manifest.version))
  }
  // Slim CI images need no Git executable: GitLab already binds the checkout to this SHA.
  const inCI = process.env.CI_COMMIT_SHA !== undefined || process.env.CI
  const sourceCommit = inCI ? process.env.CI_COMMIT_SHA ?? ''
    : execFileSync('git', ['-C', source, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim()
  assert.ok(sourceCommit.length === 40 && /^[a-f0-9]{40}$/.test(sourceCommit),
    inCI ? 'QoderWork CI_COMMIT_SHA must be a complete lowercase 40-character Git SHA' : 'QoderWork source commit must be a complete Git SHA')
  const payload = entries.map(entry => ({ path: entry.name.slice(rootName.length + 1), sha256: hash(entry.contents) }))
  add('BUILD-PROVENANCE.json', json({ schemaVersion: 1, target: 'qoderwork', version: manifest.version, sourceVersion: info.version, sourceCommit, files: payload }))
  const bytes = createZip(entries.sort((a, b) => a.name.localeCompare(b.name)))
  const archivePath = resolve(output, `${rootName}-${manifest.version}.zip`)
  const sha256 = hash(bytes)
  for (const path of [archivePath, `${archivePath}.sha256`]) {
    const exists = await lstat(path).then(() => true, error => { if (error.code === 'ENOENT') return false; throw error })
    assert.ok(!exists, `Refusing to overwrite candidate artifact: ${path}`)
  }
  await writeFile(archivePath, bytes, { flag: 'wx' })
  await writeFile(`${archivePath}.sha256`, `${sha256}  ${rootName}-${manifest.version}.zip\n`, { flag: 'wx' })
  return { archivePath, sha256, version: manifest.version }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  assert.equal(process.argv.length, 4, 'Usage: node scripts/qoderwork.mjs --output-dir <directory>')
  assert.equal(process.argv[2], '--output-dir')
  process.stdout.write(json(await buildQoderArchive({ outputDirectory: process.argv[3] })))
}

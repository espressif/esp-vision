// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { lstat, mkdir, readFile, realpath, writeFile } from 'node:fs/promises'
import { dirname, isAbsolute, relative, resolve, sep } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createZip, validatePackage } from '../src/installer.mjs'

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const templateRoot = resolve(packageRoot, 'workbuddy/expert')
const rootName = 'esp-vision-workbuddy'
const skillName = 'esp-vision-development'
const knowledgeUrl = 'https://mcp.vision.espressif.com/mcp'
const json = value => `${JSON.stringify(value, null, 2)}\n`
const digest = value => createHash('sha256').update(value).digest('hex')

function validateExpertVersion(version) {
  assert.match(version, /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/)
  assert.ok(version.length <= 20, `WorkBuddy Expert version must be at most 20 characters (got ${version.length}): ${version}`)
}

export function ciBuildIdentity(sourceVersion, env) {
  assert.match(env.CI_COMMIT_SHA ?? '', /^[a-f0-9]{40}$/, 'CI build needs the full source SHA')
  assert.match(env.CI_PIPELINE_ID ?? '', /^[1-9]\d*$/, 'CI build needs a positive pipeline ID')
  const tag = /^(?:v(\d+)\.(\d+)(?:\.(\d+))?|(\d{4})\.(\d{1,2})\.(\d{1,2}))(?:[-.]([0-9A-Za-z.-]+))?$/.exec(env.CI_COMMIT_TAG ?? '')
  let expertVersion
  if (tag) {
    const components = tag[1] ? [tag[1], tag[2], tag[3] ?? '0'] : [tag[4], tag[5], tag[6]]
    expertVersion = components.map(Number).join('.')
    if (tag[7]) expertVersion += `-${tag[7].split('.').map(part => /^\d+$/.test(part) ? String(Number(part)) : part).join('.')}`
  } else {
    expertVersion = `${sourceVersion.split(/[+-]/)[0]}-dev.${env.CI_PIPELINE_ID}`
  }
  validateExpertVersion(expertVersion)
  return { schemaVersion: 1, expertVersion, sourceVersion, sourceCommit: env.CI_COMMIT_SHA,
    releaseTag: tag ? env.CI_COMMIT_TAG : null, pipelineId: tag ? null : env.CI_PIPELINE_ID }
}

function inside(parent, path) {
  const fromParent = relative(parent, path)
  return fromParent === '' || (!isAbsolute(fromParent) && fromParent !== '..' && !fromParent.startsWith(`..${sep}`))
}

async function template(name, binary = false) {
  const path = resolve(templateRoot, name)
  assert.ok((await lstat(path)).isFile() && !(await lstat(path)).isSymbolicLink(), `Invalid Expert template: ${name}`)
  return readFile(path, binary ? undefined : 'utf8')
}

function replaceParagraph(text, prefix, replacement) {
  const paragraphs = text.split('\n\n')
  assert.equal(paragraphs.filter(paragraph => paragraph.startsWith(prefix)).length, 1,
    `Expert adapter requires exactly one paragraph starting with: ${prefix}`)
  return paragraphs.map(paragraph => paragraph.startsWith(prefix) ? replacement : paragraph).join('\n\n')
}

function frontmatter(fields) {
  // JSON scalar/flow values are valid YAML and safely quote punctuation and Unicode.
  return `---\n${Object.entries(fields).map(([key, value]) => `${key}: ${JSON.stringify(value)}`).join('\n')}\n---`
}

function adaptSkill(commonSkill, manifest) {
  const match = /^---\nname: ([^\n]+)\ndescription: ([^\n]+)\n---/.exec(commonSkill)
  assert.ok(match && match[1] === skillName, 'Expert adapter requires the common Skill name/description frontmatter')
  let body = commonSkill.slice(match[0].length)
  body = replaceParagraph(body, 'Invoke the installed plugin copy with Node',
    'Invoke Node with the bundled CLI at `${CODEBUDDY_PLUGIN_ROOT}/cli/device-cli.mjs`. Quote the resulting absolute path, including on Windows. If this host leaves the placeholder unresolved, locate this installed Skill and resolve the plugin root two directories above the `esp-vision-development` directory; verify that `cli/device-cli.mjs` exists there before running it. Do not infer the plugin root from the user project or silently run a global CLI. If the host omitted bundled resources, report the installation failure. Do not use `npx` or add a Device MCP wrapper.')
  body = body.replace('node <plugin-root>/cli/device-cli.mjs --help', 'node "${CODEBUDDY_PLUGIN_ROOT}/cli/device-cli.mjs" --help')
    .replaceAll('[application workflow](references/application-workflow.md)', '@references/application-workflow.md')
  assert.ok(body.includes('@references/application-workflow.md'), 'Expert Skill must reference the application workflow')
  assert.ok(!body.includes('<plugin-root>') && !body.includes('CLAUDE_PLUGIN_ROOT'), 'Expert Skill contains a foreign host path')
  return frontmatter({
    name: skillName, description: manifest.description,
    display_name: manifest.displayName.zh, display_name_en: manifest.displayName.en,
    description_zh: manifest.displayDescription.zh,
    description_en: manifest.displayDescription.en, version: manifest.version, author: manifest.author.name,
    examples_zh: manifest.quickPrompts.map(prompt => prompt.zh),
    examples_en: manifest.quickPrompts.map(prompt => prompt.en),
  }) + body
}

export function validateManifest(manifest) {
  validateExpertVersion(manifest.version)
  assert.equal(manifest.name, 'esp-vision')
  assert.equal(manifest.plugin, manifest.name)
  assert.equal(manifest.expertType, 'agent')
  assert.equal(manifest.agentName, skillName)
  assert.equal(manifest.author.name, 'Espressif Systems (Shanghai) CO LTD')
  assert.equal(manifest.author.email, 'libo@espressif.com')
  assert.equal(manifest.categoryId, '02-Engineering')
  assert.ok([...manifest.displayDescription.zh].length >= 40 && [...manifest.displayDescription.zh].length <= 50)
  assert.match(manifest.displayDescription.zh, /基于 ESP32/, 'Expert Chinese description must explicitly state 基于 ESP32')
  assert.ok(Array.isArray(manifest.keywords) && manifest.keywords.includes('ESP32'), 'Expert keywords must include ESP32')
  assert.ok(manifest.tags.some(tag => tag.zh?.includes('ESP32') && tag.en?.includes('ESP32')),
    'Expert visible bilingual tags must include ESP32')
  assert.equal(manifest.tags.length, 3)
  assert.equal(manifest.quickPrompts.length, 3)
  assert.deepEqual(manifest.defaultInitPrompt, manifest.quickPrompts[0])
  assert.deepEqual(manifest.agents, [`./agents/${skillName}.md`])
  assert.deepEqual(manifest.skills, [`./skills/${skillName}`])
  assert.equal(manifest.avatar, 'avatars/esp-vision.png')
  assert.deepEqual(manifest.dependencies, { mcpServers: './.mcp.json' })
  for (const value of ['displayName', 'profession', 'displayDescription', 'defaultInitPrompt']
    .map(key => manifest[key]).concat(manifest.tags, manifest.quickPrompts)) {
    assert.deepEqual(Object.keys(value).sort(), ['en', 'zh'])
    assert.ok(Object.values(value).every(text => typeof text === 'string' && text.trim()))
  }
  for (const [locale, name] of Object.entries(manifest.displayName)) {
    const length = [...name].length
    assert.ok(length <= 15, `WorkBuddy Expert displayName.${locale} must be at most 15 characters (got ${length}): ${name}`)
  }
}

/** Build a separate maintainer artifact. Never invoke a host, network API or device. */
export async function buildExpertArchive({ sourceRoot = packageRoot, outputDirectory, ciEnvironment } = {}) {
  assert.ok(outputDirectory, 'Pass --output-dir <directory> outside the plugin source')
  const source = resolve(sourceRoot)
  let output = resolve(outputDirectory)
  assert.ok(!inside(packageRoot, output) && !inside(source, output), 'Expert output must be outside both plugin source directories')
  const packageInfo = await validatePackage(source)
  const build = ciEnvironment ? ciBuildIdentity(packageInfo.version, ciEnvironment) : undefined
  const original = JSON.parse(await readFile(resolve(source, 'plugin.json'), 'utf8'))
  const manifest = { ...JSON.parse(await template('plugin.json')), version: build?.expertVersion ?? packageInfo.version }
  for (const key of ['name', 'homepage', 'license', 'keywords']) manifest[key] = original[key]
  manifest.author = { name: original.author.name, email: manifest.author.email }
  validateManifest(manifest)

  const files = new Map()
  function add(path, contents, mode = 0o644) {
    assert.ok(!path.startsWith('/') && !path.includes('\\') && !path.split('/').some(part => !part || part === '..' || part === '.'), `Unsafe Expert path: ${path}`)
    assert.ok(!files.has(path), `Duplicate Expert path: ${path}`)
    files.set(path, { name: `${rootName}/${path}`, contents: Buffer.from(contents), mode })
  }
  // Reuse the complete validated CLI closure and its license texts without rewriting.
  for (const file of packageInfo.files.filter(file => /^(cli|assets)\//.test(file.path))) {
    add(file.path, await readFile(resolve(source, file.path)), file.mode & 0o111 ? 0o755 : 0o644)
  }
  for (const name of ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'DEVICE_CORE_NOTICE.md']) {
    add(name, await readFile(resolve(source, name)))
  }
  add('.codebuddy-plugin/plugin.json', json(manifest))
  const mcp = JSON.parse(await readFile(resolve(source, '.mcp.json'), 'utf8'))
  assert.deepEqual(mcp.mcpServers, { 'esp-vision-knowledge': { type: 'http', url: knowledgeUrl } },
    'Expert export accepts only the official credential-free Knowledge MCP')
  mcp.mcpServers['esp-vision-knowledge']['x-workbuddy'] = {
    displayName: { zh: 'ESP-VISION 官方知识', en: 'ESP-VISION Knowledge' },
    description: { zh: '查询官方固件、开发板、接口和示例文档。设备操作使用专家包内的本地 CLI。',
      en: 'Query official firmware, board, API and example documentation. Device operations use the bundled local CLI.' },
    icon: './avatars/esp-vision.png', auth: { type: 'none' },
  }
  add('.mcp.json', json(mcp))
  add(`skills/${skillName}/SKILL.md`, adaptSkill(await readFile(resolve(source, `skills/${skillName}/SKILL.md`), 'utf8'), manifest))
  const reference = replaceParagraph(await readFile(resolve(source, `skills/${skillName}/references/application-workflow.md`), 'utf8'),
    'WorkBuddy currently exports',
    'This WorkBuddy Expert ZIP contains an Agent, Skill, the local CLI and a declared Knowledge MCP dependency. A prepared ZIP is not evidence that WorkBuddy has parsed, installed or activated it. Complete the dependency connection guide when summoning the expert and verify the Knowledge connection in a new conversation. Verify that the installed host retained the CLI and resolved the plugin root before device use. Knowledge MCP supplies documentation; local device operations use the bundled CLI rather than a Device MCP server. The separately published npm 0.1.0 installer still produces its documented legacy Skill ZIP and separate MCP configuration; use this Expert archive for the expert publication workflow.')
    .replace('CLI=/absolute/installed/plugin/cli/device-cli.mjs', 'CLI="${CODEBUDDY_PLUGIN_ROOT}/cli/device-cli.mjs"')
  add(`skills/${skillName}/references/application-workflow.md`, reference)
  add(`agents/${skillName}.md`, await template('agent.md'))
  add('README.md', (await template('README.md')).replaceAll('{{VERSION}}', manifest.version).replaceAll('{{SOURCE_VERSION}}', packageInfo.version))
  if (build) add('BUILD-PROVENANCE.json', json(build))

  const avatar = await template('avatar.png', true)
  assert.equal(avatar.subarray(0, 8).toString('hex'), '89504e470d0a1a0a', 'Expert avatar must be a PNG')
  assert.equal(avatar.readUInt32BE(16), 512)
  assert.equal(avatar.readUInt32BE(20), 512)
  assert.ok(avatar.length <= 500 * 1024, 'Expert avatar must not exceed 500 KB')
  add('avatars/esp-vision.png', avatar)
  add('assets/avatar-source.svg', await template('avatar-source.svg'))
  const provenance = files.get('assets/PROVENANCE.md')
  provenance.contents = Buffer.from(`${provenance.contents.toString('utf8')}\nThe Expert avatar is a 512 × 512 rasterization of the camera-symbol group from the repository's \`docs/_static/ESP-VISION_logo.svg\`, with square white padding. Its vector source is \`assets/avatar-source.svg\`; the PNG is \`avatars/esp-vision.png\`. The brand design is retained. No separate trademark or asset license is asserted.\n`)
  for (const [path, entry] of files) {
    if (/\.(md|json|svg)$/.test(path)) {
      const text = entry.contents.toString('utf8')
      assert.ok(!text.includes(source) && !text.includes('{{VERSION}}') && !/(?:\/(?:home|Users)\/[^/\s]+|\b[A-Za-z]:[\\/](?![\\/]))/.test(text), `Nonportable Expert content: ${path}`)
    }
  }
  const entries = [...files.values()].sort((left, right) => left.name < right.name ? -1 : left.name > right.name ? 1 : 0)
  const archive = createZip(entries)
  const archiveName = `esp-vision-workbuddy-expert-${manifest.version}.zip`
  const sha256 = digest(archive)
  const receipt = {
    archive: archiveName, sha256, sourceVersion: packageInfo.version, expertVersion: manifest.version, author: manifest.author,
    ...(build ? { build } : {}),
    inventory: entries.map(entry => ({ path: entry.name.slice(rootName.length + 1), bytes: entry.contents.length, sha256: digest(entry.contents) })),
    uploaded: false, targetHostVerified: false, hardwareOperations: false,
  }
  await mkdir(output, { recursive: true })
  output = await realpath(output)
  assert.ok(!inside(await realpath(packageRoot), output) && !inside(await realpath(source), output),
    'Expert output must be outside both plugin source directories, including symlink aliases')
  // Never replace an earlier artifact, checksum or receipt during review.
  for (const name of [archiveName, `${archiveName}.sha256`, `${archiveName}.receipt.json`]) {
    await assert.rejects(lstat(resolve(output, name)), error => error.code === 'ENOENT', `Artifact already exists: ${name}`)
  }
  await writeFile(resolve(output, archiveName), archive, { flag: 'wx' })
  await writeFile(resolve(output, `${archiveName}.sha256`), `${sha256}  ${archiveName}\n`, { flag: 'wx' })
  await writeFile(resolve(output, `${archiveName}.receipt.json`), json(receipt), { flag: 'wx' })
  return { ...receipt, archivePath: resolve(output, archiveName) }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const args = process.argv.slice(2)
    const options = {}
    while (args.length) {
      const option = args.shift()
      if (option === '--ci') {
        assert.ok(!options['--ci'], 'Repeated option: --ci')
        options['--ci'] = true
        continue
      }
      assert.ok(['--source', '--output-dir'].includes(option) && args[0] && !args[0].startsWith('--'),
        'Usage: npm run build:workbuddy-expert -- --output-dir DIR [--source PREBUILT_PLUGIN] [--ci]')
      assert.ok(!Object.hasOwn(options, option), `Repeated option: ${option}`)
      options[option] = args.shift()
    }
    const result = await buildExpertArchive({ sourceRoot: options['--source'], outputDirectory: options['--output-dir'],
      ciEnvironment: options['--ci'] ? process.env : undefined })
    process.stdout.write(`${json(result)}`)
  } catch (error) {
    process.stderr.write(`Expert export failed: ${error.message}\n`)
    process.exitCode = 1
  }
}

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { execFile as execFileCallback } from 'node:child_process'
import { chmod, cp, lstat, mkdir, mkdtemp, readFile, rm, symlink, writeFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { test } from 'node:test'
import { fileURLToPath } from 'node:url'
import { promisify } from 'node:util'
import { crc32, inflateRawSync } from 'node:zlib'
import { buildExpertArchive, ciBuildIdentity, validateManifest } from '../scripts/workbuddy-expert.mjs'

const execFile = promisify(execFileCallback)
const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const tempBase = resolve(process.env.TMPDIR || resolve(packageRoot, '../../.cache/expert-tests'))

function unpack(archive) {
  const entries = new Map()
  const end = archive.length - 22
  assert.equal(archive.readUInt32LE(end), 0x06054b50)
  const count = archive.readUInt16LE(end + 10)
  let offset = archive.readUInt32LE(end + 16)
  for (let index = 0; index < count; index++) {
    assert.equal(archive.readUInt32LE(offset), 0x02014b50)
    const nameLength = archive.readUInt16LE(offset + 28)
    const name = archive.subarray(offset + 46, offset + 46 + nameLength).toString('utf8')
    assert.ok(name.startsWith('esp-vision-workbuddy/') && !name.includes('\\') && !name.split('/').includes('..'))
    assert.ok(!entries.has(name) && !/\/(?:node_modules|\.git|__MACOSX|\.DS_Store)(?:\/|$)/.test(name))
    const mode = archive.readUInt32LE(offset + 38) >>> 16
    assert.equal(mode & 0o170000, 0o100000, 'ZIP entries must be regular files')
    const local = archive.readUInt32LE(offset + 42)
    assert.equal(archive.readUInt32LE(local), 0x04034b50)
    assert.equal(archive.readUInt16LE(local + 8), 8)
    const start = local + 30 + archive.readUInt16LE(local + 26) + archive.readUInt16LE(local + 28)
    const contents = inflateRawSync(archive.subarray(start, start + archive.readUInt32LE(offset + 20)))
    assert.equal(contents.length, archive.readUInt32LE(offset + 24))
    assert.equal(crc32(contents), archive.readUInt32LE(offset + 16), `CRC mismatch: ${name}`)
    entries.set(name.slice('esp-vision-workbuddy/'.length), { contents, mode })
    offset += 46 + nameLength + archive.readUInt16LE(offset + 30) + archive.readUInt16LE(offset + 32)
  }
  assert.equal(offset, end, 'ZIP must contain exactly one central directory and no trailing data')
  return entries
}

async function temporary(t) {
  await mkdir(tempBase, { recursive: true })
  const path = await mkdtemp(resolve(tempBase, 'workbuddy-expert-'))
  t.after(() => rm(path, { recursive: true, force: true }))
  return path
}

test('Expert archive roundtrips deterministically with portable Agent/Skill/MCP and original CLI/licenses', async t => {
  const root = await temporary(t)
  const first = await buildExpertArchive({ outputDirectory: resolve(root, 'first') })
  const second = await buildExpertArchive({ outputDirectory: resolve(root, 'second') })
  assert.equal(first.sha256, second.sha256)
  const archive = await readFile(first.archivePath)
  const entries = unpack(archive)
  const text = path => entries.get(path).contents.toString('utf8')
  const manifest = JSON.parse(text('.codebuddy-plugin/plugin.json'))
  assert.deepEqual(manifest.author, { name: 'Espressif Systems (Shanghai) CO LTD', email: 'libo@espressif.com' })
  assert.equal(manifest.version, first.sourceVersion)
  assert.equal(manifest.expertType, 'agent')
  assert.equal(manifest.agentName, 'esp-vision-development')
  assert.deepEqual(manifest.displayName, { en: 'ESP-Vision AI', zh: 'ESP-Vision AI视觉' })
  assert.ok(Object.values(manifest.displayName).every(name => [...name].length <= 15))
  assert.match(manifest.displayDescription.zh, /^基于 ESP32/)
  assert.ok(manifest.keywords.includes('ESP32'))
  assert.ok(manifest.tags.some(tag => tag.en === 'ESP32' && tag.zh === 'ESP32'))
  assert.equal(manifest.tags.length, 3)
  assert.equal(manifest.quickPrompts.length, 3)
  assert.deepEqual(manifest.defaultInitPrompt, manifest.quickPrompts[0])
  for (const path of [...manifest.agents, ...manifest.skills.map(path => `${path}/SKILL.md`), manifest.avatar, manifest.dependencies.mcpServers]) {
    assert.ok(entries.has(path.replace(/^\.\//, '')), `Missing manifest target: ${path}`)
  }
  const agent = text('agents/esp-vision-development.md')
  assert.ok(agent.startsWith('---\n'))
  assert.ok(agent.includes(`\nname: ${manifest.agentName}\n`))
  assert.ok(agent.includes(`\ndescription: ${manifest.description}\n`))
  for (const field of ['displayName', 'profession']) {
    const yaml = Object.entries(manifest[field]).map(([locale, value]) => `  ${locale}: ${value}\n`).join('')
    assert.ok(agent.includes(`\n${field}:\n${yaml}`), `Agent ${field} must match the manifest`)
  }
  assert.match(text('agents/esp-vision-development.md'), /skills:\n- esp-vision-development/)
  assert.doesNotMatch(text('agents/esp-vision-development.md'), /^tools:/m)
  const skill = text('skills/esp-vision-development/SKILL.md')
  assert.ok(skill.includes(`display_name: ${JSON.stringify(manifest.displayName.zh)}`))
  assert.ok(skill.includes(`display_name_en: ${JSON.stringify(manifest.displayName.en)}`))
  for (const [locale, name] of Object.entries(manifest.displayName)) {
    assert.ok(text('agents/esp-vision-development.md').includes(`  ${locale}: ${name}\n`))
  }
  for (const [field, value] of Object.entries({
    description: manifest.description, description_zh: manifest.displayDescription.zh,
    description_en: manifest.displayDescription.en,
    examples_zh: manifest.quickPrompts.map(prompt => prompt.zh),
    examples_en: manifest.quickPrompts.map(prompt => prompt.en),
  })) assert.ok(skill.includes(`\n${field}: ${JSON.stringify(value)}\n`), `Missing Skill ${field}`)
  assert.match(skill, /author: "Espressif Systems \(Shanghai\) CO LTD"/)
  assert.ok(skill.includes(`version: ${JSON.stringify(manifest.version)}`))
  assert.match(skill, /\$\{CODEBUDDY_PLUGIN_ROOT\}/)
  assert.match(skill, /@references\/application-workflow\.md/)
  assert.doesNotMatch(skill, /\[application workflow\]\(references\/application-workflow\.md\)/)
  assert.match(skill, /Search existing issues first/)
  assert.match(skill, /English GitHub issue draft/)
  assert.match(skill, /explicit user authorization before submitting or uploading/)
  assert.doesNotMatch(skill, /Offer the user three reporting choices|official anonymous submission tool/)
  const reference = text('skills/esp-vision-development/references/application-workflow.md')
  assert.match(reference, /Troubleshooting Q&A/)
  assert.match(skill, /prefer PowerShell/)
  assert.match(reference, /MSYS2_ARG_CONV_EXCL='\*' node/)
  assert.match(reference, /node "\$CLI" files list --path \/models/)
  assert.match(reference, /not a confirmed explanation for every `INVALID_PATH`/)
  assert.match(reference, /Never remove a drive letter/)
  assert.match(reference, /does not access a device/)
  assert.doesNotMatch(reference, /export MSYS2_ARG_CONV_EXCL/)

  assert.match(reference, /--python py/)
  assert.match(reference, /not proof that every serial error has the same cause/)
  assert.match(reference, /ENUMERATION_UNAVAILABLE.*not that no device exists/)
  assert.match(reference, /do not force takeover/)
  assert.match(reference, /Do not repeatedly retry or switch tools\/interpreters to evade a denial/)
  assert.match(reference, /Knowledge MCP provides read-only documentation queries/)
  assert.match(reference, /https:\/\/github\.com\/espressif\/esp-vision\/security\/policy/)

  assert.doesNotMatch(skill, /CLAUDE_PLUGIN_ROOT|<plugin-root>|\/home\//)
  assert.match(text('skills/esp-vision-development/references/application-workflow.md'), /Expert ZIP contains an Agent/)
  const mcp = JSON.parse(text('.mcp.json')).mcpServers
  assert.deepEqual(Object.keys(mcp), ['esp-vision-knowledge'])
  assert.equal(mcp['esp-vision-knowledge'].url, 'https://mcp.vision.espressif.com/mcp')
  assert.deepEqual(mcp['esp-vision-knowledge']['x-workbuddy'].auth, { type: 'none' })
  const metadata = mcp['esp-vision-knowledge']['x-workbuddy']
  assert.equal(metadata.icon, `./${manifest.avatar}`)
  assert.ok(entries.has(metadata.icon.slice(2)), 'MCP icon resolves from the root .mcp.json')
  for (const field of ['displayName', 'description']) {
    assert.deepEqual(Object.keys(metadata[field]).sort(), ['en', 'zh'])
    assert.ok(Object.values(metadata[field]).every(value => typeof value === 'string' && value.trim()))
  }
  const avatar = entries.get(manifest.avatar).contents
  assert.equal(avatar.readUInt32BE(16), 512)
  assert.equal(avatar.readUInt32BE(20), 512)
  assert.ok(avatar.length < 500 * 1024)
  for (const record of first.inventory.filter(record => record.path.startsWith('cli/') || ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'DEVICE_CORE_NOTICE.md'].includes(record.path))) {
    assert.deepEqual(entries.get(record.path).contents, await readFile(resolve(packageRoot, record.path)), record.path)
  }
  assert.equal(first.uploaded, false)
  assert.equal(first.targetHostVerified, false)
  assert.equal(first.hardwareOperations, false)
  assert.equal(entries.get('cli/device-cli.mjs').mode & 0o100, 0o100)

  const extracted = resolve(root, 'extracted expert with spaces')
  for (const [path, { contents }] of entries) {
    await mkdir(dirname(resolve(extracted, path)), { recursive: true })
    await writeFile(resolve(extracted, path), contents)
  }
  const env = { ...process.env, PATH: `${dirname(process.execPath)}${process.platform === 'win32' ? ';' : ':'}${process.env.PATH}` }
  delete env.NODE_PATH
  delete env.NODE_OPTIONS
  const cli = resolve(extracted, 'cli/device-cli.mjs')
  const { stdout: help } = await execFile(process.execPath, [cli, '--help'], { env, cwd: root, timeout: 15000 })
  assert.match(help, /esp-vision-device inspect/)
  if (process.platform !== 'win32') {
    const fakePython = resolve(root, 'fake python inventory')
    await writeFile(fakePython, '#!/usr/bin/env node\nif (!process.argv[2]?.replaceAll("\\\\", "/").endsWith("/node_serial.py")) process.exit(91)\nprocess.stdout.write("[]\\n")\n')
    await chmod(fakePython, 0o755)
    const { stdout, stderr } = await execFile(process.execPath, [cli, 'inspect', '--interface', 'serial', '--python', fakePython, '--json'], { env, cwd: root, timeout: 15000 })
    const result = JSON.parse(stdout)
    assert.equal(result.ok, true)
    assert.deepEqual(result.result.devices, [])
    assert.equal(result.result.sourcesAvailable, 1)
    assert.ok(stderr.trim().split('\n').map(line => JSON.parse(line)).every(event => event.schemaVersion === 1 && typeof event.event === 'string'))
  }
})

test('Expert export rejects replacement and source-nested destinations without modifying a prior artifact', async t => {
  const root = await temporary(t)
  const result = await buildExpertArchive({ outputDirectory: root })
  const original = await readFile(result.archivePath)
  await assert.rejects(buildExpertArchive({ outputDirectory: root }), /Artifact already exists/)
  assert.deepEqual(await readFile(result.archivePath), original)
  await assert.rejects(buildExpertArchive({ outputDirectory: resolve(packageRoot, 'workbuddy') }), /outside both plugin source/)
})

test('Expert export rejects credential-bearing MCP or a symlinked CLI instead of repackaging it', async t => {
  const root = await temporary(t)
  const fixture = resolve(root, 'source')
  await cp(packageRoot, fixture, { recursive: true })
  const path = resolve(fixture, '.mcp.json')
  const original = await readFile(path)
  const mcp = JSON.parse(original)
  mcp.mcpServers['esp-vision-knowledge'].headers = { Authorization: 'must-not-be-exported' }
  await writeFile(path, JSON.stringify(mcp))
  await assert.rejects(buildExpertArchive({ sourceRoot: fixture, outputDirectory: resolve(root, 'credentials') }), /credential-free Knowledge/)
  await writeFile(path, original)
  const helper = resolve(fixture, 'cli/node_serial.py')
  await rm(helper)
  if (process.platform !== 'win32') {
    await symlink(resolve(packageRoot, 'cli/node_serial.py'), helper)
    await assert.rejects(buildExpertArchive({ sourceRoot: fixture, outputDirectory: resolve(root, 'symlink') }), /symlink/)
  }
})

test('Expert names accept 15 characters per locale and reject 16 and the reported 18-character name', async () => {
  const manifest = { ...JSON.parse(await readFile(resolve(packageRoot, 'workbuddy/expert/plugin.json'), 'utf8')), version: '0.1.0' }
  for (const locale of ['zh', 'en']) {
    const accepted = { ...manifest, displayName: { ...manifest.displayName, [locale]: 'x'.repeat(15) } }
    assert.doesNotThrow(() => validateManifest(accepted))
    const rejected = { ...accepted, displayName: { ...accepted.displayName, [locale]: 'x'.repeat(16) } }
    assert.throws(() => validateManifest(rejected), new RegExp(`displayName\\.${locale}.*at most 15 characters \\(got 16\\)`))
  }
  const reported = { ...manifest, displayName: { ...manifest.displayName, zh: 'ESP-Vision AI 视觉开发' } }
  assert.throws(() => validateManifest(reported), /displayName\.zh.*at most 15 characters \(got 18\)/)
})

test('CI identities distinguish unpublished candidates, fixed release tags and unchanged npm versions', async t => {
  const env = { CI_COMMIT_SHA: 'a'.repeat(40), CI_PIPELINE_ID: '123' }
  assert.equal(ciBuildIdentity('0.1.0', env).expertVersion, '0.1.0-dev.123')
  assert.notEqual(ciBuildIdentity('0.1.0', { ...env, CI_PIPELINE_ID: '124' }).expertVersion, ciBuildIdentity('0.1.0', env).expertVersion)
  assert.equal(ciBuildIdentity('0.1.0', { ...env, CI_PIPELINE_ID: '1730541' }).expertVersion, '0.1.0-dev.1730541')
  assert.equal(ciBuildIdentity('0.1.0', { ...env, CI_COMMIT_TAG: 'device-core-v0.5.1' }).releaseTag, null)
  assert.equal(ciBuildIdentity('0.1.0', { ...env, CI_COMMIT_TAG: 'v1.2' }).expertVersion, '1.2.0')
  assert.equal(ciBuildIdentity('0.1.0', { ...env, CI_COMMIT_TAG: '2026.09.01-rc.1' }).expertVersion, '2026.9.1-rc.1')
  assert.throws(() => ciBuildIdentity('0.1.0', { ...env, CI_COMMIT_SHA: 'short' }), /full source SHA/)
  const root = await temporary(t)
  const candidate = await buildExpertArchive({ outputDirectory: resolve(root, 'candidate'), ciEnvironment: env })
  assert.equal(candidate.sourceVersion, '0.1.0')
  assert.equal(candidate.expertVersion, '0.1.0-dev.123')
  const entries = unpack(await readFile(candidate.archivePath))
  const provenance = entries.get('BUILD-PROVENANCE.json').contents
  assert.deepEqual(JSON.parse(provenance), candidate.build)
  assert.equal(candidate.build.sourceCommit, env.CI_COMMIT_SHA)
  assert.equal(candidate.build.pipelineId, env.CI_PIPELINE_ID)
  assert.equal(JSON.parse(entries.get('.codebuddy-plugin/plugin.json').contents).version, candidate.expertVersion)
  assert.ok(entries.get('skills/esp-vision-development/SKILL.md').contents.toString().includes(`version: "${candidate.expertVersion}"`))
  const releaseEnv = { ...env, CI_COMMIT_TAG: '2026.10.27' }
  const first = await buildExpertArchive({ outputDirectory: resolve(root, 'release1'), ciEnvironment: releaseEnv })
  const retry = await buildExpertArchive({ outputDirectory: resolve(root, 'release2'), ciEnvironment: { ...releaseEnv, CI_PIPELINE_ID: '124' } })
  assert.equal(first.expertVersion, '2026.10.27')
  assert.equal(first.sourceVersion, '0.1.0')
  assert.equal(first.build.pipelineId, null, 'release identity must not change on a pipeline retry')
  assert.equal(first.sha256, retry.sha256)
})

test('CI candidates and normalized release tags accept 20 characters and reject 21 before output', async t => {
  const env = { CI_COMMIT_SHA: 'a'.repeat(40), CI_PIPELINE_ID: '1234567890' }
  assert.equal(ciBuildIdentity('0.1.0', env).expertVersion.length, 20)
  const tag = `v1.2.3-${'a'.repeat(14)}`
  assert.equal(ciBuildIdentity('0.1.0', { ...env, CI_COMMIT_TAG: tag }).expertVersion.length, 20)
  const root = await temporary(t)
  for (const [name, ciEnvironment] of [
    ['candidate', { ...env, CI_PIPELINE_ID: '12345678901' }],
    ['release', { ...env, CI_COMMIT_TAG: `${tag}a` }],
  ]) {
    const outputDirectory = resolve(root, name)
    await assert.rejects(buildExpertArchive({ outputDirectory, ciEnvironment }), /at most 20 characters \(got 21\)/)
    await assert.rejects(lstat(outputDirectory), { code: 'ENOENT' })
  }
})

test('local Expert versions enforce the same 20-character limit without truncation', async t => {
  const root = await temporary(t)
  const sourceRoot = resolve(root, 'source')
  await cp(packageRoot, sourceRoot, { recursive: true })
  const version = `1.2.3-${'a'.repeat(14)}`
  async function setVersion(value) {
    for (const name of ['package.json', 'plugin.json', '.claude-plugin/plugin.json']) {
      const path = resolve(sourceRoot, name)
      const manifest = JSON.parse(await readFile(path, 'utf8'))
      await writeFile(path, JSON.stringify({ ...manifest, version: value }))
    }
  }
  await setVersion(version)
  const result = await buildExpertArchive({ sourceRoot, outputDirectory: resolve(root, 'valid') })
  assert.equal(result.expertVersion, version)
  await setVersion(`${version}a`)
  const outputDirectory = resolve(root, 'oversized')
  await assert.rejects(buildExpertArchive({ sourceRoot, outputDirectory }), /at most 20 characters \(got 21\)/)
  await assert.rejects(lstat(outputDirectory), { code: 'ENOENT' })
})

test('Expert manifest rejects missing ESP32 scope, keywords and bilingual discovery tags', async () => {
  const manifest = { ...JSON.parse(await readFile(resolve(packageRoot, 'workbuddy/expert/plugin.json'), 'utf8')), version: '0.1.0' }
  for (const length of [40, 50]) {
    assert.doesNotThrow(() => validateManifest({ ...manifest, displayDescription: { ...manifest.displayDescription, zh: '基于 ESP32' + 'x'.repeat(length - 8) } }))
  }
  for (const [patch, error] of [
    [{ displayDescription: { ...manifest.displayDescription, zh: '视觉应用开发专家'.repeat(6) } }, /基于 ESP32/],
    [{ displayDescription: { ...manifest.displayDescription, zh: '基于 ESP32' + 'x'.repeat(31) } }, /assert/],
    [{ displayDescription: { ...manifest.displayDescription, zh: '基于 ESP32' + 'x'.repeat(43) } }, /assert/],
    [{ keywords: ['MicroPython'] }, /keywords/],
    [{ tags: [{ zh: 'ESP32', en: 'Vision' }] }, /bilingual tags/],
    [{ tags: [{ zh: '视觉', en: 'ESP32' }] }, /bilingual tags/],
  ]) assert.throws(() => validateManifest({ ...manifest, ...patch }), error)
})

test('Expert archive bytes ignore source umask while preserving executable payloads', async t => {
  const root = await temporary(t)
  const sourceRoot = resolve(root, 'source')
  await cp(packageRoot, sourceRoot, { recursive: true })
  const paths = ['assets/esp-vision-logo.png', 'cli/node_serial.py', 'cli/licenses/ansi-regex-license']
  for (const path of paths) await chmod(resolve(sourceRoot, path), 0o600)
  await chmod(resolve(sourceRoot, 'cli/device-cli.mjs'), 0o700)
  const first = await buildExpertArchive({ sourceRoot, outputDirectory: resolve(root, 'first') })
  for (const path of paths) await chmod(resolve(sourceRoot, path), 0o666)
  await chmod(resolve(sourceRoot, 'cli/device-cli.mjs'), 0o775)
  const second = await buildExpertArchive({ sourceRoot, outputDirectory: resolve(root, 'second') })
  assert.equal(first.sha256, second.sha256)
  const entries = unpack(await readFile(second.archivePath))
  for (const path of paths) assert.equal(entries.get(path).mode & 0o777, 0o644)
  assert.equal(entries.get('cli/device-cli.mjs').mode & 0o777, 0o755)
})

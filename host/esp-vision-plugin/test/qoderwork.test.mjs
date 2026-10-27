// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { execFileSync, spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { chmod, cp, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { test } from 'node:test'
import { fileURLToPath } from 'node:url'
import { inflateRawSync } from 'node:zlib'
import { adaptQoderSkill, buildQoderArchive, validateQoderManifest } from '../scripts/qoderwork.mjs'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const tempBase = resolve(process.env.TMPDIR ?? resolve(root, '../../.cache/qoderwork-tests'))
function unpack(bytes) {
  const files = new Map()
  assert.equal(bytes.readUInt32LE(bytes.length - 22), 0x06054b50)
  let offset = bytes.readUInt32LE(bytes.length - 6)
  const count = bytes.readUInt16LE(bytes.length - 12)
  for (let i = 0; i < count; i++) {
    assert.equal(bytes.readUInt32LE(offset), 0x02014b50)
    const nameLength = bytes.readUInt16LE(offset + 28)
    const name = bytes.subarray(offset + 46, offset + 46 + nameLength).toString()
    assert.ok(name.startsWith('esp-vision-qoderwork/') && !name.includes('\\') && !name.split('/').includes('..'))
    const local = bytes.readUInt32LE(offset + 42)
    const start = local + 30 + bytes.readUInt16LE(local + 26) + bytes.readUInt16LE(local + 28)
    const value = inflateRawSync(bytes.subarray(start, start + bytes.readUInt32LE(offset + 20)))
    assert.equal(value.length, bytes.readUInt32LE(offset + 24))
    assert.ok(!files.has(name))
    files.set(name.slice('esp-vision-qoderwork/'.length), value)
    offset += 46 + nameLength + bytes.readUInt16LE(offset + 30) + bytes.readUInt16LE(offset + 32)
  }
  return files
}

test('QoderWork candidate roundtrips deterministically with original runtime/license bytes', async t => {
  await mkdir(tempBase, { recursive: true })
  const temp = await mkdtemp(resolve(tempBase, 'qoderwork-'))
  t.after(() => rm(temp, { recursive: true, force: true }))
  const first = await buildQoderArchive({ outputDirectory: resolve(temp, 'path with spaces') })
  const second = await buildQoderArchive({ outputDirectory: resolve(temp, 'second') })
  assert.equal(first.sha256, second.sha256)
  await assert.rejects(buildQoderArchive({ outputDirectory: resolve(temp, 'path with spaces') }), /overwrite/)
  const orphan = resolve(temp, 'orphan-checksum')
  await mkdir(orphan)
  await writeFile(resolve(orphan, 'esp-vision-qoderwork-0.1.0.zip.sha256'), 'preserved')
  await assert.rejects(buildQoderArchive({ outputDirectory: orphan }), /overwrite/)
  assert.equal(await readFile(resolve(orphan, 'esp-vision-qoderwork-0.1.0.zip.sha256'), 'utf8'), 'preserved')
  const bytes = await readFile(first.archivePath)
  assert.equal(createHash('sha256').update(bytes).digest('hex'), first.sha256)
  const files = unpack(bytes)
  const text = name => files.get(name).toString()
  const manifest = JSON.parse(text('.qoder-plugin/plugin.json'))
  validateQoderManifest(manifest)
  assert.equal(manifest.version, '0.1.0')
  assert.ok(!files.has('.codebuddy-plugin/plugin.json') && !files.has('.claude-plugin/plugin.json'))
  assert.ok(![...files.keys()].some(name => /node_modules|\.git\/|workbuddy\//.test(name)))
  const mcp = JSON.parse(text('.mcp.json'))
  assert.deepEqual(Object.keys(mcp.mcpServers), ['esp-vision-knowledge'])
  assert.equal(mcp.mcpServers['esp-vision-knowledge'].type, 'streamable-http')
  assert.equal(mcp.mcpServers['esp-vision-knowledge'].url, '{{USER_CONFIG}}')
  assert.match(mcp.mcpServers['esp-vision-knowledge']._setup.guide, /https:\/\/mcp\.vision\.espressif\.com\/mcp/)
  assert.doesNotMatch(text('skills/esp-vision-development/SKILL.md'), /CODEBUDDY_PLUGIN_ROOT|CLAUDE_PLUGIN_ROOT|`PLUGIN_ROOT`|<plugin-root>|\/absolute\/installed/)
  assert.match(text('skills/esp-vision-development/SKILL.md'), /^version: 0\.1\.0$/m)
  const skill = text('skills/esp-vision-development/SKILL.md')
  const reference = text('skills/esp-vision-development/references/application-workflow.md')
  assert.match(skill, /Troubleshooting Q&A in \[application workflow\]/)
  assert.match(skill, /Search existing issues first/)
  assert.match(skill, /English GitHub issue draft/)
  assert.match(skill, /explicit user authorization before submitting or uploading/)
  assert.doesNotMatch(skill, /Offer the user three reporting choices|official anonymous submission tool/)
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
  assert.match(reference, /Knowledge MCP provides read-only documentation queries/)
  assert.match(reference, /https:\/\/github\.com\/espressif\/esp-vision\/security\/policy/)

  assert.doesNotMatch(text('skills/esp-vision-development/references/application-workflow.md'), /WorkBuddy|CodeBuddy|CODEBUDDY_PLUGIN_ROOT|CLAUDE_PLUGIN_ROOT/)
  for (const path of ['README.md', 'skills/esp-vision-development/SKILL.md', 'skills/esp-vision-development/references/application-workflow.md']) {
    assert.doesNotMatch(text(path), /<实际安装路径>|<plugin-root>|\/absolute\/installed|\/dev\/selected-device/)
    assert.match(text(path), /CLI:\?Set CLI to the verified installed bundle path/)
  }
  assert.match(text('qoder.md'), /设备会话、授权、脚本中断和验收/)
  assert.match(text('CONNECTORS.md'), /尚无平台证据/)
  const provenance = JSON.parse(text('BUILD-PROVENANCE.json'))
  assert.match(provenance.sourceCommit, /^[a-f0-9]{40}$/)
  for (const entry of provenance.files) {
    assert.equal(createHash('sha256').update(files.get(entry.path)).digest('hex'), entry.sha256)
    if (/^cli\//.test(entry.path) || ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'DEVICE_CORE_NOTICE.md'].includes(entry.path)) {
      assert.deepEqual(files.get(entry.path), await readFile(resolve(root, entry.path)), entry.path)
    }
  }
  // Python's independent ZIP reader verifies CRCs and retains directory layout.
  execFileSync('python3', ['-c', 'import sys,zipfile; z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None; z.extractall(sys.argv[2])', first.archivePath, resolve(temp, 'extracted')])
  const help = execFileSync(process.execPath, [resolve(temp, 'extracted/esp-vision-qoderwork/cli/device-cli.mjs'), '--help'], { encoding: 'utf8' })
  assert.match(help, /esp-vision-device inspect/)
})

test('QoderWork fails closed on invalid names, versions and undeclared dependencies', async () => {
  const manifest = JSON.parse(await readFile(resolve(root, 'qoderwork/plugin.json'), 'utf8'))
  for (const version of ['0.1.0-dev.1', '01.1.0', '1.0', '../unsafe']) {
    assert.throws(() => validateQoderManifest({ ...manifest, version }))
  }
  assert.throws(() => validateQoderManifest({ ...manifest, skills: ['../foreign'] }))
  assert.throws(() => validateQoderManifest({ ...manifest, mcpServers: '../foreign' }))
  assert.throws(() => adaptQoderSkill('unrecognized skill format', '0.1.0'))
  await assert.rejects(buildQoderArchive({ outputDirectory: root }), /outside/)
})


test('QoderWork builds without Git on PATH using validated CI_COMMIT_SHA', async t => {
  await mkdir(tempBase, { recursive: true })
  const temp = await mkdtemp(resolve(tempBase, 'qoderwork-no-git-'))
  t.after(() => rm(temp, { recursive: true, force: true }))
  const sourceCommit = '0123456789abcdef0123456789abcdef01234567'
  const script = resolve(root, 'scripts/qoderwork.mjs')
  const outputDirectory = resolve(temp, 'valid')
  const run = (output, sha) => {
    const env = { ...process.env, PATH: '', CI: 'true' }
    if (sha === undefined) delete env.CI_COMMIT_SHA
    else env.CI_COMMIT_SHA = sha
    return spawnSync(process.execPath, [script, '--output-dir', output], { env, encoding: 'utf8' })
  }
  const built = run(outputDirectory, sourceCommit)
  assert.equal(built.status, 0, built.stderr)
  const result = JSON.parse(built.stdout)
  const files = unpack(await readFile(result.archivePath))
  assert.equal(JSON.parse(files.get('BUILD-PROVENANCE.json')).sourceCommit, sourceCommit)
  for (const [index, invalid] of ['', undefined, 'abc123', sourceCommit.toUpperCase(), `${sourceCommit}\n`].entries()) {
    const output = resolve(temp, `invalid-${index}`)
    const rejected = run(output, invalid)
    assert.notEqual(rejected.status, 0)
    assert.match(rejected.stderr, /CI_COMMIT_SHA must be a complete lowercase 40-character Git SHA/)
    assert.doesNotMatch(rejected.stderr, /ENOENT/)
    await assert.rejects(readFile(resolve(output, 'esp-vision-qoderwork-0.1.0.zip')), { code: 'ENOENT' })
  }
})


test('QoderWork archive permissions are deterministic across source umasks', { skip: process.platform === 'win32' }, async t => {
  await mkdir(tempBase, { recursive: true })
  const temp = await mkdtemp(resolve(tempBase, 'qoderwork-modes-'))
  t.after(() => rm(temp, { recursive: true, force: true }))
  const source = resolve(temp, 'source')
  await cp(root, source, { recursive: true })
  const nonexecutables = ['assets/esp-vision-logo.png', 'cli/node_serial.py']
  // Choose an existing license rather than assuming a specific vendored license filename.
  const provenanceBuild = async output => JSON.parse(execFileSync(process.execPath, ['--input-type=module', '--eval',
    `import { buildQoderArchive } from ${JSON.stringify(new URL('../scripts/qoderwork.mjs', import.meta.url).href)}; console.log(JSON.stringify(await buildQoderArchive({sourceRoot:process.argv[1],outputDirectory:process.argv[2]})))`, source, output],
  { env: { ...process.env, CI_COMMIT_SHA: '0123456789abcdef0123456789abcdef01234567' }, encoding: 'utf8' }))
  const initial = await provenanceBuild(resolve(temp, 'initial'))
  const initialFiles = unpack(await readFile(initial.archivePath))
  nonexecutables.push([...initialFiles.keys()].find(path => path.startsWith('cli/licenses/')))
  const executable = 'cli/device-cli.mjs'
  for (const path of nonexecutables) await chmod(resolve(source, path), 0o600)
  await chmod(resolve(source, executable), 0o700)
  const restricted = await provenanceBuild(resolve(temp, 'restricted'))
  for (const path of nonexecutables) await chmod(resolve(source, path), 0o666)
  await chmod(resolve(source, executable), 0o775)
  const permissive = await provenanceBuild(resolve(temp, 'permissive'))
  assert.equal(initial.sha256, restricted.sha256)
  assert.equal(restricted.sha256, permissive.sha256)
  const bytes = await readFile(permissive.archivePath)
  let offset = bytes.readUInt32LE(bytes.length - 6)
  for (let index = 0; index < bytes.readUInt16LE(bytes.length - 12); index++) {
    const nameLength = bytes.readUInt16LE(offset + 28)
    const path = bytes.subarray(offset + 46, offset + 46 + nameLength).toString().slice('esp-vision-qoderwork/'.length)
    const mode = bytes.readUInt32LE(offset + 38) >>> 16
    assert.equal(mode & 0o777, path === executable ? 0o755 : 0o644, path)
    offset += 46 + nameLength + bytes.readUInt16LE(offset + 30) + bytes.readUInt16LE(offset + 32)
  }
})

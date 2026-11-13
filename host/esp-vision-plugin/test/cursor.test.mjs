// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { execFileSync, spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { chmod, cp, mkdir, mkdtemp, readFile, rm, symlink, writeFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { test } from 'node:test'
import { fileURLToPath } from 'node:url'
import { adaptCursorSkill, buildCursorArchive } from '../scripts/cursor.mjs'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const tempBase = resolve(process.env.TMPDIR ?? resolve(root, '../../.cache/cursor-tests'))
const hash = bytes => createHash('sha256').update(bytes).digest('hex')
async function workspace(t) {
  await mkdir(tempBase, { recursive: true })
  const directory = await mkdtemp(resolve(tempBase, 'cursor-'))
  t.after(() => rm(directory, { recursive: true, force: true }))
  return directory
}

test('Cursor ZIP retains complete runtime, metadata and guidance and runs after independent extraction', async t => {
  const temp = await workspace(t)
  const first = await buildCursorArchive({ outputDirectory: resolve(temp, 'one') })
  const second = await buildCursorArchive({ outputDirectory: resolve(temp, 'two') })
  assert.equal(first.sha256, second.sha256)
  assert.equal(hash(await readFile(first.archivePath)), first.sha256)
  assert.equal(await readFile(`${first.archivePath}.sha256`, 'utf8'), `${first.sha256}  esp-vision-cursor-${first.version}.zip\n`)
  const extracted = resolve(temp, 'plugin with spaces')
  const inventory = JSON.parse(execFileSync('python3', ['-c', [
    'import sys,json,zipfile,stat',
    'z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None',
    'assert len(z.namelist()) == len(set(z.namelist()))',
    'assert all(i.filename.startswith("esp-vision/") and ".." not in i.filename.split("/") and "\\\\" not in i.filename and stat.S_ISREG(i.external_attr >> 16) and (i.external_attr >> 16) & 0o777 in (0o644,0o755) for i in z.infolist())',
    'z.extractall(sys.argv[2]); print(json.dumps(z.namelist()))',
  ].join('\n'), first.archivePath, extracted], { encoding: 'utf8' }))
  const plugin = resolve(extracted, 'esp-vision')
  const text = name => readFile(resolve(plugin, name), 'utf8')
  const manifest = JSON.parse(await text('.cursor-plugin/plugin.json'))
  assert.equal(manifest.name, 'esp-vision')
  assert.equal(manifest.displayName, 'ESP-Vision 嵌入式 AI 视觉开发')
  assert.equal(manifest.version, first.version)
  assert.deepEqual(manifest.skills, ['./skills/esp-vision-development'])
  assert.equal(manifest.mcpServers, './mcp.json')
  assert.equal(manifest.logo, 'assets/esp-vision-logo.png')
  assert.deepEqual(JSON.parse(await text('mcp.json')), { mcpServers: { 'esp-vision-knowledge': { url: 'https://mcp.vision.espressif.com/mcp' } } })
  assert.ok(inventory.every(name => !/\.claude-plugin|\.codebuddy-plugin|\.qoder-plugin|node_modules|(^|\/)\.git\//.test(name)))
  const skill = await text('skills/esp-vision-development/SKILL.md')
  assert.match(skill, /Never silently fall back to latest/)
  assert.match(skill, /Only after the user explicitly declines the firmware update/)
  assert.match(skill, /--declined-firmware-update/)
  assert.match(skill, /result\.identity\.firmwareVersion/)
  assert.match(await text('skills/esp-vision-development/scripts/knowledge-version.mjs'), /FIRMWARE_VERSION_REQUIRED/)
  const reference = await text('skills/esp-vision-development/references/application-workflow.md')
  assert.doesNotMatch(skill + reference, /WorkBuddy|CodeBuddy|CLAUDE_PLUGIN_ROOT|CODEBUDDY_PLUGIN_ROOT|<plugin-root>|\/absolute\/installed|\/dev\/selected-device/)
  assert.match(skill, /not a guaranteed environment variable/)
  assert.match(skill, /Search existing issues first/)
  assert.match(skill, /explicit user authorization before submitting or uploading/)
  assert.match(skill, /MSYS2_ARG_CONV_EXCL/)
  assert.match(reference, /No host Python is required/)
  assert.match(reference, /Knowledge MCP provides read-only documentation queries/)
  assert.match(await text('README.md'), /~\/\.cursor\/plugins\/local\//)
  assert.match(await text('README.md'), /cursor\.com\/marketplace\/publish/)
  const provenance = JSON.parse(await text('BUILD-PROVENANCE.json'))
  assert.equal(provenance.target, 'cursor')
  assert.equal(provenance.sourceCommit, first.sourceCommit)
  assert.equal(provenance.version, manifest.version)
  assert.deepEqual(provenance.files.map(item => `esp-vision/${item.path}`).sort(), inventory.filter(name => !name.endsWith('/BUILD-PROVENANCE.json')).sort())
  for (const item of provenance.files) {
    const bytes = await readFile(resolve(plugin, item.path))
    assert.equal(hash(bytes), item.sha256, item.path)
    if (/^(cli|assets)\//.test(item.path) || ['LICENSE', 'DEVICE_CORE_NOTICE.md', 'THIRD_PARTY_NOTICES.md'].includes(item.path)) {
      assert.deepEqual(bytes, await readFile(resolve(root, item.path)), item.path)
    }
  }
  const cli = resolve(plugin, 'cli/device-cli.mjs')
  const help = execFileSync(process.execPath, [cli, '--help'], { encoding: 'utf8', cwd: temp })
  assert.match(help, /esp-vision-device inspect/)
  const rejected = spawnSync(process.execPath, [cli, 'files', 'list', '--port', 'COM9', '--path', 'C:/Program Files/Git/models', '--json', '--python', 'py'], { encoding: 'utf8', cwd: temp })
  assert.equal(rejected.status, 2, rejected.stderr)
  assert.match(JSON.parse(rejected.stdout).error.message, /MSYS2_ARG_CONV_EXCL/)
  assert.doesNotMatch(rejected.stderr, /connecting/)
  {
    const result = JSON.parse(execFileSync(process.execPath, [cli, 'inspect', '--interface', 'serial', '--json'], { encoding: 'utf8', cwd: temp }))
    assert.equal(result.ok, true)
    assert.ok(Array.isArray(result.result.devices))
    assert.equal(result.result.sourcesAvailable, 1)
  }
})

test('Cursor export preserves previous artifacts and rejects source/output aliases', async t => {
  const temp = await workspace(t)
  const outputDirectory = resolve(temp, 'export')
  const first = await buildCursorArchive({ outputDirectory })
  await assert.rejects(buildCursorArchive({ outputDirectory }), /overwrite/)
  assert.equal(hash(await readFile(first.archivePath)), first.sha256)
  const orphan = resolve(temp, 'orphan')
  await mkdir(orphan)
  await writeFile(resolve(orphan, `esp-vision-cursor-${first.version}.zip.sha256`), 'preserve me')
  await assert.rejects(buildCursorArchive({ outputDirectory: orphan }), /overwrite/)
  await assert.rejects(readFile(resolve(orphan, `esp-vision-cursor-${first.version}.zip`)), { code: 'ENOENT' })
  assert.equal(await readFile(resolve(orphan, `esp-vision-cursor-${first.version}.zip.sha256`), 'utf8'), 'preserve me')
  await assert.rejects(buildCursorArchive({ outputDirectory: root }), /outside the plugin source/)
  if (process.platform !== 'win32') {
    const alias = resolve(temp, 'source-alias')
    await symlink(root, alias, 'dir')
    await assert.rejects(buildCursorArchive({ outputDirectory: alias }), /outside the plugin source/)
  }
  assert.throws(() => adaptCursorSkill('foreign instructions'), /AssertionError/)
})

test('Cursor CI builds without Git and rejects incomplete or noncanonical source identities before writing ZIPs', async t => {
  const temp = await workspace(t)
  const emptyPath = resolve(temp, 'no-executables')
  await mkdir(emptyPath)
  const entry = resolve(root, 'scripts/cursor.mjs')
  const sourceCommit = 'a'.repeat(40)
  const env = { ...process.env, PATH: emptyPath, CI: 'true', CI_COMMIT_SHA: sourceCommit }
  const result = spawnSync(process.execPath, [entry, '--output-dir', resolve(temp, 'ci')], { encoding: 'utf8', env })
  assert.equal(result.status, 0, result.stderr)
  assert.equal(JSON.parse(result.stdout).sourceCommit, sourceCommit)
  for (const [index, value] of ['', 'abc', 'A'.repeat(40), sourceCommit + '\n', undefined].entries()) {
    const invalidEnv = { ...env, CI_COMMIT_SHA: value }
    if (value === undefined) delete invalidEnv.CI_COMMIT_SHA
    const output = resolve(temp, `bad-${index}`)
    const rejected = spawnSync(process.execPath, [entry, '--output-dir', output], { encoding: 'utf8', env: invalidEnv })
    assert.notEqual(rejected.status, 0)
    assert.match(rejected.stderr, /complete lowercase 40-character Git SHA/)
    assert.doesNotMatch(rejected.stderr, /ENOENT/)
    await assert.rejects(readFile(resolve(output, 'esp-vision-cursor-0.1.0.zip')), { code: 'ENOENT' })
  }
})


test('Cursor ZIP identity is unaffected by non-executable or executable source permission variations', { skip: process.platform === 'win32' }, async t => {
  const temp = await workspace(t)
  const source = resolve(temp, 'source')
  await cp(root, source, { recursive: true })
  const logo = resolve(source, 'assets/esp-vision-logo.png')
  const cli = resolve(source, 'cli/device-cli.mjs')
  const options = { sourceRoot: source, ciEnvironment: { CI: 'true', CI_COMMIT_SHA: 'b'.repeat(40) } }
  await chmod(logo, 0o600)
  await chmod(cli, 0o700)
  const first = await buildCursorArchive({ ...options, outputDirectory: resolve(temp, 'mode-one') })
  await chmod(logo, 0o666)
  await chmod(cli, 0o775)
  const second = await buildCursorArchive({ ...options, outputDirectory: resolve(temp, 'mode-two') })
  assert.equal(first.sha256, second.sha256)
})

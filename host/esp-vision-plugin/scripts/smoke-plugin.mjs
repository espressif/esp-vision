// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { chmod, cp, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { promisify } from 'node:util'
import { execFile as execFileCallback } from 'node:child_process'
import { inflateRawSync } from 'node:zlib'
import { installPlugin } from '../src/installer.mjs'

const execFile = promisify(execFileCallback)
const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')

function pluginDirectory() {
  const args = process.argv.slice(2)
  const index = args.indexOf('--plugin-dir')
  if (index < 0) return packageRoot
  if (!args[index + 1] || args.length !== 2) throw new Error('Usage: node scripts/smoke-plugin.mjs [--plugin-dir <directory>]')
  return resolve(args[index + 1])
}

function unzipSkill(buffer) {
  const entries = []
  let offset = 0
  while (offset + 4 <= buffer.length && buffer.readUInt32LE(offset) === 0x04034b50) {
    const method = buffer.readUInt16LE(offset + 8)
    const compressedSize = buffer.readUInt32LE(offset + 18)
    const fileNameLength = buffer.readUInt16LE(offset + 26)
    const extraLength = buffer.readUInt16LE(offset + 28)
    const fileName = buffer.subarray(offset + 30, offset + 30 + fileNameLength).toString('utf8')
    const dataStart = offset + 30 + fileNameLength + extraLength
    const compressed = buffer.subarray(dataStart, dataStart + compressedSize)
    if (method !== 8 || !fileName || fileName.startsWith('/') || fileName.split('/').includes('..')) {
      throw new Error('WorkBuddy Skill ZIP contains an unsupported or unsafe entry.')
    }
    entries.push({ name: fileName, contents: inflateRawSync(compressed) })
    offset = dataStart + compressedSize
  }
  if (entries.length === 0 || offset >= buffer.length) throw new Error('WorkBuddy Skill ZIP is empty or malformed.')
  return entries
}

const sourcePlugin = pluginDirectory()
const tempBase = resolve(process.env.TMPDIR || resolve(packageRoot, '../../.cache/plugin-smoke'))
await mkdir(tempBase, { recursive: true })
const tempRoot = await mkdtemp(resolve(tempBase, 'esp-vision-plugin-smoke-'))

try {
  const isolatedPlugin = resolve(tempRoot, 'plugin')
  await cp(sourcePlugin, isolatedPlugin, { recursive: true })
  const manifest = JSON.parse(await readFile(resolve(isolatedPlugin, 'plugin.json'), 'utf8'))
  assert.equal(manifest.name, 'esp-vision')
  const cli = resolve(isolatedPlugin, 'cli/device-cli.mjs')
  const env = { ...process.env, NODE_NO_WARNINGS: '1' }
  delete env.NODE_PATH
  delete env.NODE_OPTIONS

  const { stdout: help } = await execFile(process.execPath, [cli, '--help'], {
    cwd: isolatedPlugin, env, timeout: 15000, maxBuffer: 2 * 1024 * 1024, windowsHide: true,
  })
  assert.match(help, /esp-vision-device inspect/)
  assert.match(help, /esp-vision-device python/)

  if (process.platform === 'win32') {
    process.stdout.write('Isolated CLI help smoke passed; injected serial runner smoke is skipped on Windows.\n')
  } else {
    const fakePython = resolve(tempRoot, 'fake-python')
    await writeFile(fakePython, '#!/usr/bin/env node\nif (!process.argv[2]?.replaceAll("\\\\", "/").endsWith("/node_serial.py")) process.exit(91)\nprocess.stdout.write("[]\\n")\n')
    await chmod(fakePython, 0o755)
    const { stdout, stderr } = await execFile(process.execPath, [
      cli, 'inspect', '--interface', 'serial', '--python', fakePython, '--json',
    ], {
      cwd: isolatedPlugin, env, timeout: 15000, maxBuffer: 2 * 1024 * 1024, windowsHide: true,
    })
    const result = JSON.parse(stdout)
    assert.deepEqual(Object.keys(result), ['schemaVersion', 'ok', 'command', 'result'])
    assert.equal(result.schemaVersion, 1)
    assert.equal(result.ok, true)
    assert.equal(result.command, 'inspect')
    assert.deepEqual(result.result.devices, [])
    assert.equal(result.result.sourcesAvailable, 1)
    const stderrEvents = stderr.trim().split('\n').map(line => JSON.parse(line))
    assert.ok(stderrEvents.length >= 1)
    assert.ok(stderrEvents.every(event => event.schemaVersion === 1 && typeof event.event === 'string'))
    process.stdout.write('Isolated CLI help and inspect smoke passed with the injected serial runner.\n')
  }

  const workbuddyData = resolve(tempRoot, 'workbuddy data with spaces')
  const workbuddy = await installPlugin({ client: 'workbuddy', dataDirectory: workbuddyData, packageRoot: isolatedPlugin })
  assert.equal(workbuddy.installationState, 'manual-install-required')
  assert.equal(workbuddy.dryRun, false)
  const mcp = JSON.parse(await readFile(workbuddy.mcpConfig, 'utf8'))
  assert.deepEqual(Object.keys(mcp.mcpServers), ['esp-vision-knowledge'])
  assert.equal(mcp.mcpServers['esp-vision-knowledge'].type, 'streamableHttp')
  assert.equal(mcp.mcpServers['esp-vision-knowledge'].url, 'https://mcp.vision.espressif.com/mcp')
  const zipEntries = unzipSkill(await readFile(workbuddy.skillZip))
  const skillFiles = new Map(zipEntries.map(entry => [entry.name, entry.contents]))
  const skillRoot = 'esp-vision-development/'
  assert.ok(skillFiles.has(`${skillRoot}SKILL.md`))
  assert.ok(skillFiles.has(`${skillRoot}scripts/cli/device-cli.mjs`))
  assert.ok(skillFiles.has(`${skillRoot}scripts/cli/node_serial.py`))
  assert.ok(skillFiles.has(`${skillRoot}assets/esp-vision-logo.png`))
  assert.ok(skillFiles.has(`${skillRoot}assets/PROVENANCE.md`))
  assert.ok(skillFiles.has(`${skillRoot}references/application-workflow.md`))
  const exportedSkill = skillFiles.get(`${skillRoot}SKILL.md`).toString('utf8')
  assert.doesNotMatch(exportedSkill, /PLUGIN_ROOT|CLAUDE_PLUGIN_ROOT|<plugin-root>/)
  const extractedSkill = resolve(tempRoot, 'extracted WorkBuddy skill')
  for (const { name, contents } of zipEntries) {
    const destination = resolve(extractedSkill, ...name.split('/'))
    await mkdir(dirname(destination), { recursive: true })
    await writeFile(destination, contents)
  }
  const extractedCli = resolve(extractedSkill, 'esp-vision-development/scripts/cli/device-cli.mjs')
  const { stdout: extractedHelp } = await execFile(process.execPath, [extractedCli, '--help'], {
    cwd: tempRoot, env, timeout: 15000, maxBuffer: 2 * 1024 * 1024, windowsHide: true,
  })
  assert.match(extractedHelp, /esp-vision-device inspect/)
  if (process.platform !== 'win32') {
    const fakePython = resolve(tempRoot, 'fake-python')
    const { stdout: extractedInspect } = await execFile(process.execPath, [
      extractedCli, 'inspect', '--interface', 'serial', '--python', fakePython, '--json',
    ], {
      cwd: tempRoot, env, timeout: 15000, maxBuffer: 2 * 1024 * 1024, windowsHide: true,
    })
    const result = JSON.parse(extractedInspect)
    assert.equal(result.ok, true)
    assert.equal(result.command, 'inspect')
    assert.deepEqual(result.result.devices, [])
  }
  process.stdout.write('WorkBuddy export and extracted Skill CLI smoke passed; UI activation remains manual.\n')
} finally {
  await rm(tempRoot, { recursive: true, force: true })
}

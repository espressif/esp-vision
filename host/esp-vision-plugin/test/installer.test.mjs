// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { after, before, test } from 'node:test'
import { constants } from 'node:fs'
import { chmod, copyFile, cp, lstat, mkdir, mkdtemp, readFile, readdir, rm, symlink, writeFile } from 'node:fs/promises'
import { dirname, join, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { execFile as execFileCallback } from 'node:child_process'
import { promisify } from 'node:util'
import { inflateRawSync } from 'node:zlib'
import {
  defaultDataDirectory,
  installPlugin,
  parseInstallArgs,
  validatePackage,
} from '../src/installer.mjs'

const execFile = promisify(execFileCallback)

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const repositoryRoot = resolve(packageRoot, '../..')
const packageManifest = JSON.parse(await readFile(join(packageRoot, 'package.json'), 'utf8'))
const packageVersion = packageManifest.version
// Synthetic prior version for upgrade regression; no plugin was publicly released.
const previousVersion = '0.0.0'
const configuredTempRoot = [process.env.TMPDIR, process.env.TMP, process.env.TEMP]
  .find(path => {
    if (!path) return false
    const fromRepository = relative(repositoryRoot, resolve(path))
    return fromRepository !== '' && fromRepository !== '..' && !fromRepository.startsWith(`..${process.platform === 'win32' ? '\\' : '/'}`)
  })
const testDataRoot = configuredTempRoot ? resolve(configuredTempRoot) : join(repositoryRoot, '.cache', 'plugin-installer-tests')
let testRoot

before(async () => {
  await mkdir(testDataRoot, { recursive: true })
  testRoot = await mkdtemp(join(testDataRoot, 'installer-test-'))
})

after(async () => {
  if (testRoot) await rm(testRoot, { recursive: true, force: true })
})

function createRunner({ client = 'codex', failAt, calls = [], marketplaces = [], plugins = [] } = {}) {
  const state = { marketplaces: structuredClone(marketplaces), plugins: structuredClone(plugins) }
  let mutationCount = 0
  const runner = async (command, args) => {
    calls.push({ command, args })
    if (args[0] === 'plugin' && args[1] === 'marketplace' && args[2] === 'list') {
      return { stdout: client === 'codex' ? JSON.stringify({ marketplaces: state.marketplaces }) : JSON.stringify(state.marketplaces) }
    }
    if (args[0] === 'plugin' && args[1] === 'list') {
      return { stdout: client === 'codex' ? JSON.stringify({ installed: state.plugins, available: [] }) : JSON.stringify(state.plugins) }
    }
    mutationCount += 1
    if (mutationCount === failAt) throw Object.assign(new Error('private child stderr must not leak'), { status: 17 })
    if ((client === 'codex' && args[0] === 'plugin' && args[1] === 'remove')
      || (client === 'claude' && args[0] === 'plugin' && args[1] === 'uninstall')) {
      state.plugins = state.plugins.filter(row => row.id !== 'esp-vision@esp-vision' && row.pluginId !== 'esp-vision@esp-vision')
    } else if (args[0] === 'plugin' && args[1] === 'marketplace' && args[2] === 'add') {
      const installPath = args[3]
      state.marketplaces = state.marketplaces.filter(row => row.name !== 'esp-vision')
      state.marketplaces.push(client === 'codex'
        ? { name: 'esp-vision', root: installPath, marketplaceSource: { sourceType: 'local', source: installPath } }
        : { name: 'esp-vision', installLocation: installPath, source: { source: 'local', path: installPath } })
    } else if (args[0] === 'plugin' && args[1] === 'marketplace' && args[2] === 'remove') {
      state.marketplaces = state.marketplaces.filter(row => row.name !== 'esp-vision')
    } else if ((client === 'codex' && args[0] === 'plugin' && args[1] === 'add')
      || (client === 'claude' && args[0] === 'plugin' && args[1] === 'install')) {
      const installedPath = client === 'codex'
        ? state.marketplaces.find(row => row.name === 'esp-vision')?.root
        : state.marketplaces.find(row => row.name === 'esp-vision')?.installLocation
      state.plugins = state.plugins.filter(row => row.id !== 'esp-vision@esp-vision' && row.pluginId !== 'esp-vision@esp-vision')
      state.plugins.push(client === 'codex'
        ? { name: 'esp-vision', marketplaceName: 'esp-vision', pluginId: 'esp-vision@esp-vision', source: { sourceType: 'local', source: installedPath }, version: packageVersion }
        : { id: 'esp-vision@esp-vision', installPath: installedPath, scope: 'user', version: packageVersion })
    }
    return { stdout: '', stderr: '' }
  }
  runner.calls = calls
  runner.state = state
  return runner
}

function mutations(calls) {
  return calls.filter(({ args }) => !(args[0] === 'plugin' && (
    (args[1] === 'marketplace' && args[2] === 'list') || (args[1] === 'list')
  )))
}

async function targetDirectory(dataDirectory, version = packageVersion) {
  return resolve(dataDirectory, 'plugins', version)
}

async function createOwnedPreviousInstall(directory, version = previousVersion) {
  await mkdir(directory, { recursive: true })
  for (const entry of [
    '.agents', '.claude-plugin', '.mcp.json', 'mcp.json', 'plugin.json',
    'DEVICE_CORE_NOTICE.md', 'LICENSE', 'README.md', 'THIRD_PARTY_NOTICES.md',
    'assets', 'cli', 'skills', 'workbuddy', 'bin', 'src', 'package.json',
  ]) {
    await cp(join(packageRoot, entry), join(directory, entry), { recursive: true })
  }
  for (const entry of ['package.json', 'plugin.json', '.claude-plugin/plugin.json']) {
    const manifest = JSON.parse(await readFile(join(directory, entry), 'utf8'))
    manifest.version = version
    await writeFile(join(directory, entry), `${JSON.stringify(manifest, null, 2)}\n`)
  }
  const { bundleHash } = await validatePackage(directory)
  await writeFile(join(directory, '.esp-vision-install.json'), `${JSON.stringify({
    name: '@esp-vision/plugin', version, bundleHash,
  }, null, 2)}\n`)
}

function unzipSkill(buffer) {
  const entries = []
  let offset = 0
  while (offset + 4 <= buffer.length && buffer.readUInt32LE(offset) === 0x04034b50) {
    const method = buffer.readUInt16LE(offset + 8)
    const compressedSize = buffer.readUInt32LE(offset + 18)
    const nameLength = buffer.readUInt16LE(offset + 26)
    const extraLength = buffer.readUInt16LE(offset + 28)
    const name = buffer.subarray(offset + 30, offset + 30 + nameLength).toString('utf8')
    const start = offset + 30 + nameLength + extraLength
    const compressed = buffer.subarray(start, start + compressedSize)
    assert.equal(method, 8)
    assert.ok(name && !name.startsWith('/') && !name.split('/').includes('..'))
    entries.push({ name, contents: inflateRawSync(compressed) })
    offset = start + compressedSize
  }
  assert.ok(entries.length > 0)
  return entries
}

async function extractSkill(zipPath, destination) {
  const entries = unzipSkill(await readFile(zipPath))
  for (const entry of entries) {
    const path = resolve(destination, ...entry.name.split('/'))
    await mkdir(dirname(path), { recursive: true })
    await writeFile(path, entry.contents)
  }
  return entries
}

test('package metadata declares a zero-dependency CLI and complete npm allowlist', async () => {
  const bundle = await validatePackage(packageRoot)
  const manifest = JSON.parse(await readFile(join(packageRoot, 'package.json'), 'utf8'))
  const binInfo = await lstat(join(packageRoot, 'bin/esp-vision-plugin.mjs'))
  assert.equal(bundle.version, packageVersion)
  assert.match(bundle.bundleHash, /^[a-f0-9]{64}$/)
  assert.deepEqual(manifest.bin, { 'esp-vision-plugin': 'bin/esp-vision-plugin.mjs' })
  assert.deepEqual(manifest.dependencies ?? {}, {})
  assert.deepEqual(manifest.publishConfig, { access: 'public', registry: 'https://registry.npmjs.org/' })
  assert.equal(manifest.repository.directory, 'host/esp-vision-plugin')
  assert.ok(binInfo.mode & (constants.S_IXUSR | constants.S_IXGRP | constants.S_IXOTH))
  assert.ok(!manifest.files.includes('test'), 'installer tests stay out of the published runtime package')
assert.ok(bundle.files.some(file => file.path === 'cli/device-cli.mjs'))
const licenseFiles = await readdir(join(packageRoot, 'cli/licenses'))
assert.ok(licenseFiles.some(name => name.includes('multicast-dns')), 'the standalone CLI dependency licenses are present')
})

test('parses Codex, Claude, and WorkBuddy install commands and optional paths with spaces', () => {
  assert.deepEqual(parseInstallArgs(['install', 'codex']), { client: 'codex', dryRun: false })
  assert.deepEqual(parseInstallArgs(['install', 'claude', '--dry-run', '--data-dir', '/path with spaces']), {
    client: 'claude', dryRun: true, dataDirectory: '/path with spaces',
  })
  assert.deepEqual(parseInstallArgs(['install', 'workbuddy', '--data-dir', '/path with spaces']), {
    client: 'workbuddy', dryRun: false, dataDirectory: '/path with spaces',
  })
  assert.equal(parseInstallArgs(['--help']).help, true)
})

test('rejects malformed install arguments without invoking commands', () => {
  const malformed = [
    [],
    ['install'],
    ['install', 'vscode'],
    ['install', 'workbuddy', '--unknown'],
    ['install', 'codex', '--unknown'],
    ['install', 'codex', '--data-dir'],
    ['install', 'codex', '--data-dir', '--dry-run'],
    ['install', 'codex', '--data-dir', '/one', '--data-dir', '/two'],
    ['install', 'claude', '--dry-run', '--dry-run'],
  ]
  for (const args of malformed) {
    assert.throws(() => parseInstallArgs(args), error => error.code === 'INVALID_ARGUMENTS')
  }
})

test('chooses stable per-user data locations without changing client home paths', () => {
  assert.equal(
    defaultDataDirectory({ platform: 'linux', env: { XDG_DATA_HOME: '/user/data' }, home: '/user/home' }),
    '/user/data/esp-vision',
  )
  assert.equal(
    defaultDataDirectory({ platform: 'darwin', env: {}, home: '/user/home' }),
    '/user/home/Library/Application Support/Espressif/ESP-VISION',
  )
  assert.equal(
    defaultDataDirectory({ platform: 'win32', env: { LOCALAPPDATA: 'C:\\Users\\person\\AppData\\Local' }, home: 'C:\\Users\\person' }),
    'C:\\Users\\person\\AppData\\Local\\Espressif\\ESP-VISION',
  )
})

test('rejects an incomplete package bundle before creating paths or running client commands', async () => {
  const invalidRoot = join(testRoot, 'incomplete package')
  await mkdir(invalidRoot, { recursive: true })
  await writeFile(join(invalidRoot, 'package.json'), JSON.stringify({
    name: '@esp-vision/plugin', version: packageVersion, files: [
      'bin/esp-vision-plugin.mjs', 'src/installer.mjs', '.agents/plugins/marketplace.json',
      '.claude-plugin/marketplace.json', '.claude-plugin/plugin.json', '.mcp.json',
      'assets', 'DEVICE_CORE_NOTICE.md', 'LICENSE', 'README.md', 'THIRD_PARTY_NOTICES.md',
      'mcp.json', 'plugin.json', 'cli', 'skills', 'workbuddy', 'test',
    ],
  }))
  await mkdir(join(invalidRoot, 'bin'), { recursive: true })
  await mkdir(join(invalidRoot, 'src'), { recursive: true })
  await writeFile(join(invalidRoot, 'bin/esp-vision-plugin.mjs'), 'bin')
  await writeFile(join(invalidRoot, 'src/installer.mjs'), 'source')
  const dataDirectory = join(testRoot, 'incomplete install')
  const calls = []

  await assert.rejects(installPlugin({
    client: 'codex', dataDirectory, packageRoot: invalidRoot, commandRunner: createRunner({ calls }),
  }), error => error.code === 'MISSING_ASSET' && error.message.includes('.agents/plugins/marketplace.json'))
  await assert.rejects(lstat(dataDirectory), error => error.code === 'ENOENT')
  assert.deepEqual(calls, [])
})

test('WorkBuddy installation prepares a local ZIP and MCP file, then reports manual activation', async () => {
  const dataDirectory = join(testRoot, 'workbuddy data root with spaces')
  const commandRunner = async () => { throw new Error('WorkBuddy must not invoke a native registration command') }
  const first = await installPlugin({ client: 'workbuddy', dataDirectory, packageRoot, commandRunner })

  assert.equal(first.installationState, 'manual-install-required')
  assert.equal(first.copied, true)
  assert.equal(first.exported, true)
  assert.equal(first.installDirectory, await targetDirectory(dataDirectory))
  assert.equal(first.workbuddyDirectory, resolve(dataDirectory, 'workbuddy', packageVersion))
  assert.ok(first.skillZip.includes('workbuddy data root with spaces'))
  assert.equal((await lstat(join(first.installDirectory, 'assets/esp-vision-logo.png'))).isFile(), true)
  const connector = JSON.parse(await readFile(first.mcpConfig, 'utf8'))
  assert.deepEqual(Object.keys(connector), ['mcpServers'])
  assert.deepEqual(Object.keys(connector.mcpServers), ['esp-vision-knowledge'])
  assert.deepEqual(connector.mcpServers['esp-vision-knowledge'], {
    type: 'streamableHttp', url: 'https://mcp.vision.espressif.com/mcp', timeout: 30000,
  })
  const entries = unzipSkill(await readFile(first.skillZip))
  const files = new Map(entries.map(entry => [entry.name, entry.contents]))
  const prefix = 'esp-vision-development/'
  const skill = files.get(`${prefix}SKILL.md`).toString('utf8')
  const portableCliPath = `${first.installDirectory.replaceAll('\\', '/')}/cli/device-cli.mjs`
  assert.ok(skill.includes(portableCliPath))
  assert.ok(skill.includes(`Use \`node "${portableCliPath}" --help\` for command syntax.`))
  assert.match(skill, /@references\/application-workflow\.md/)
  assert.doesNotMatch(skill, /\[application workflow\]/)
  assert.match(files.get(`${prefix}references/application-workflow.md`).toString('utf8'), /Troubleshooting Q&A/)
  assert.match(skill, /English GitHub issue draft/)
  assert.doesNotMatch(skill, /Offer the user three reporting choices/)
  assert.doesNotMatch(skill, /PLUGIN_ROOT|CLAUDE_PLUGIN_ROOT|<plugin-root>/)
  assert.ok(files.has(`${prefix}scripts/cli/device-cli.mjs`))
  assert.ok(files.has(`${prefix}scripts/cli/node_serial.py`))
  assert.ok(files.has(`${prefix}scripts/cli/node_rom.py`))
  assert.ok(files.has(`${prefix}assets/esp-vision-logo.png`))
  assert.deepEqual(files.get(`${prefix}assets/esp-vision-logo.png`), await readFile(join(packageRoot, 'assets/esp-vision-logo.png')))
  assert.ok(files.has(`${prefix}assets/PROVENANCE.md`))
  assert.ok(files.has(`${prefix}references/application-workflow.md`))
  assert.match(files.get(`${prefix}THIRD_PARTY_NOTICES.md`).toString('utf8'), /scripts\/cli\/licenses\//)
  assert.doesNotMatch(files.get(`${prefix}THIRD_PARTY_NOTICES.md`).toString('utf8'), /`cli\/licenses\/`/)
  await assert.rejects(lstat(join(dataDirectory, '.workbuddy')), error => error.code === 'ENOENT')

  const second = await installPlugin({
    client: 'workbuddy', dataDirectory, packageRoot, commandRunner,
    copyFileImpl: async () => { throw new Error('Repeat installation must not copy the payload') },
    writeFileImpl: async () => { throw new Error('Repeat installation must not stage or replace a verified export') },
  })
  assert.equal(second.installationState, 'manual-install-required')
  assert.equal(second.copied, false)
  assert.equal(second.exported, false)
  assert.deepEqual(await readFile(second.skillZip), await readFile(first.skillZip))
  assert.equal((await readdir(first.workbuddyDirectory)).length, 4)
})

test('WorkBuddy dry-run shows stable export paths without writing files', async () => {
  const dataDirectory = join(testRoot, 'workbuddy dry run path with spaces')
  const result = await installPlugin({ client: 'workbuddy', dataDirectory, packageRoot, dryRun: true })
  assert.equal(result.dryRun, true)
  assert.equal(result.installationState, 'manual-install-required')
  assert.equal(result.copied, true)
  assert.equal(result.exported, true)
  assert.equal(result.workbuddyDirectory, resolve(dataDirectory, 'workbuddy', packageVersion))
  await assert.rejects(lstat(dataDirectory), error => error.code === 'ENOENT')
})

test('WorkBuddy version upgrade leaves prior immutable plugin and export files in place', async () => {
  const dataDirectory = join(testRoot, 'workbuddy upgrade data')
  const oldPlugin = await targetDirectory(dataDirectory, previousVersion)
  await mkdir(oldPlugin, { recursive: true })
  await createOwnedPreviousInstall(oldPlugin)
  const oldCli = await readFile(join(oldPlugin, 'cli/device-cli.mjs'))
  const oldWorkbuddy = join(dataDirectory, 'workbuddy', previousVersion)
  await mkdir(oldWorkbuddy, { recursive: true })
  await writeFile(join(oldWorkbuddy, 'previous-export.txt'), 'keep previous WorkBuddy package')

  const result = await installPlugin({ client: 'workbuddy', dataDirectory, packageRoot })
  assert.equal(result.copied, true)
  assert.equal(result.exported, true)
  assert.deepEqual(await readFile(join(oldPlugin, 'cli/device-cli.mjs')), oldCli)
  assert.equal(await readFile(join(oldWorkbuddy, 'previous-export.txt'), 'utf8'), 'keep previous WorkBuddy package')
  assert.equal(JSON.parse(await readFile(join(oldPlugin, '.esp-vision-install.json'), 'utf8')).version, previousVersion)
  assert.equal(JSON.parse(await readFile(join(result.installDirectory, '.esp-vision-install.json'), 'utf8')).version, packageVersion)
})

test('WorkBuddy export failures clean staging data and a rerun safely completes preparation', async () => {
  const dataDirectory = join(testRoot, 'workbuddy export recovery')
  let writes = 0
  await assert.rejects(installPlugin({
    client: 'workbuddy', dataDirectory, packageRoot,
    writeFileImpl: async (...args) => {
      writes += 1
      if (writes === 2) throw new Error('private export failure')
      return writeFile(...args)
    },
  }), error => error.code === 'WORKBUDDY_EXPORT_FAILED'
    && error.state.workbuddyExport === 'incomplete'
    && !error.message.includes('private export failure'))

  assert.equal((await lstat(await targetDirectory(dataDirectory))).isDirectory(), true)
  const workbuddyParent = join(dataDirectory, 'workbuddy')
  assert.deepEqual(await readdir(workbuddyParent), [])
  const recovered = await installPlugin({ client: 'workbuddy', dataDirectory, packageRoot })
  assert.equal(recovered.copied, false)
  assert.equal(recovered.exported, true)
  assert.equal(recovered.installationState, 'manual-install-required')
})

test('WorkBuddy refuses to overwrite modified export artifacts and can recover after explicit removal', async () => {
  const dataDirectory = join(testRoot, 'workbuddy modified export')
  const result = await installPlugin({ client: 'workbuddy', dataDirectory, packageRoot })
  await writeFile(result.mcpConfig, '{"modified":true}\n')
  await assert.rejects(installPlugin({ client: 'workbuddy', dataDirectory, packageRoot }), error => {
    assert.equal(error.code, 'WORKBUDDY_EXPORT_MODIFIED')
    assert.match(error.message, /it was left unchanged/)
    assert.match(error.message, /Remove that export directory to recover/)
    return true
  })
  assert.equal(await readFile(result.mcpConfig, 'utf8'), '{"modified":true}\n')
  await rm(result.workbuddyDirectory, { recursive: true })
  const recovered = await installPlugin({ client: 'workbuddy', dataDirectory, packageRoot })
  assert.equal(recovered.copied, false)
  assert.equal(recovered.exported, true)
  assert.equal(JSON.parse(await readFile(recovered.mcpConfig, 'utf8')).mcpServers['esp-vision-knowledge'].type, 'streamableHttp')
})

test('the exported WorkBuddy ZIP extracts and its bundled CLI runs help and mocked serial inspection', async () => {
  const dataDirectory = join(testRoot, 'workbuddy extracted CLI data')
  const result = await installPlugin({ client: 'workbuddy', dataDirectory, packageRoot })
  const extracted = join(testRoot, 'WorkBuddy ZIP extracted with spaces')
  const entries = await extractSkill(result.skillZip, extracted)
  assert.ok(entries.length > 10)
  const extractedCli = join(extracted, 'esp-vision-development', 'scripts', 'cli', 'device-cli.mjs')
  const { stdout: help } = await execFile(process.execPath, [extractedCli, '--help'], {
    cwd: testRoot, timeout: 15000, maxBuffer: 2 * 1024 * 1024, windowsHide: true,
  })
  assert.match(help, /esp-vision-device inspect/)
  if (process.platform !== 'win32') {
    const fakePython = join(testRoot, 'workbuddy-fake-python')
    await writeFile(fakePython, '#!/usr/bin/env node\nif (!process.argv[2]?.replaceAll("\\\\", "/").endsWith("/node_serial.py")) process.exit(91)\nprocess.stdout.write("[]\\n")\n')
    await chmod(fakePython, 0o755)
    const { stdout } = await execFile(process.execPath, [
      extractedCli, 'inspect', '--interface', 'serial', '--python', fakePython, '--json',
    ], {
      cwd: testRoot, timeout: 15000, maxBuffer: 2 * 1024 * 1024, windowsHide: true,
    })
    const inspection = JSON.parse(stdout)
    assert.equal(inspection.ok, true)
    assert.equal(inspection.command, 'inspect')
    assert.deepEqual(inspection.result.devices, [])
    assert.equal(inspection.result.sourcesAvailable, 1)
  }
})

test('dry-run validates the bundle and shows both native commands without writes or commands', async () => {
  const dataDirectory = join(testRoot, 'dry run path with spaces')
  const calls = []
  const plan = await installPlugin({
    client: 'codex', dataDirectory, dryRun: true, packageRoot, commandRunner: createRunner({ calls }),
  })

  assert.equal(plan.dryRun, true)
  assert.equal(plan.installDirectory, await targetDirectory(dataDirectory))
  assert.deepEqual(plan.commands, [
    { command: 'codex', args: ['plugin', 'marketplace', 'add', plan.installDirectory] },
    { command: 'codex', args: ['plugin', 'add', 'esp-vision@esp-vision'] },
  ])
  await assert.rejects(lstat(dataDirectory), error => error.code === 'ENOENT')
  assert.deepEqual(calls, [])
})

test('installs both clients from a stable copy outside the package and npx cache', async () => {
  for (const client of ['codex', 'claude']) {
    const dataDirectory = join(testRoot, `${client} data with spaces`)
    const calls = []
    const result = await installPlugin({
      client, dataDirectory, packageRoot, commandRunner: createRunner({ client, calls }),
    })

    assert.equal(result.installDirectory, await targetDirectory(dataDirectory))
    assert.notEqual(result.installDirectory, packageRoot)
    assert.ok(result.installDirectory.startsWith(`${resolve(dataDirectory)}/`))
    assert.equal(result.copied, true)
    assert.equal(result.marketplaceState, 'completed')
    assert.equal(result.pluginState, 'completed')
    if (client === 'codex') {
      assert.deepEqual(mutations(calls), [
        { command: 'codex', args: ['plugin', 'marketplace', 'add', result.installDirectory] },
        { command: 'codex', args: ['plugin', 'add', 'esp-vision@esp-vision'] },
      ])
    } else {
      assert.deepEqual(mutations(calls), [
        { command: 'claude', args: ['plugin', 'marketplace', 'add', result.installDirectory] },
        { command: 'claude', args: ['plugin', 'install', 'esp-vision@esp-vision'] },
      ])
    }
    const installedCli = await readFile(join(result.installDirectory, 'cli/device-cli.mjs'), 'utf8')
    assert.match(installedCli, /^#!\/usr\/bin\/env node/)
    const marker = JSON.parse(await readFile(join(result.installDirectory, '.esp-vision-install.json'), 'utf8'))
    assert.equal(marker.name, '@esp-vision/plugin')
    assert.equal(marker.version, packageVersion)
  }
})

test('repeat install is idempotent and skips both copying and native registration', async () => {
  const dataDirectory = join(testRoot, 'idempotent install')
  const runner = createRunner({ client: 'claude' })
  let copyCount = 0
  const trackedCopy = async (...args) => {
    copyCount += 1
    return copyFile(...args)
  }
  const first = await installPlugin({
    client: 'claude', dataDirectory, packageRoot, commandRunner: runner, copyFileImpl: trackedCopy,
  })
  const firstCopyCount = copyCount
  const second = await installPlugin({
    client: 'claude', dataDirectory, packageRoot, commandRunner: runner, copyFileImpl: trackedCopy,
  })

  assert.equal(first.copied, true)
  assert.equal(second.copied, false)
  assert.equal(second.installDirectory, first.installDirectory)
  assert.equal(copyCount, firstCopyCount)
  assert.equal(second.marketplaceState, 'already-registered')
  assert.equal(second.pluginState, 'already-installed')
  assert.equal(mutations(runner.calls).length, 2, 'only the first install registers the marketplace and plugin')
})

test('upgrades an older installer-owned marketplace using native state and preserves the prior version', async () => {
  for (const client of ['codex', 'claude']) {
    const dataDirectory = join(testRoot, `${client} old-version data`)
    const oldDirectory = await targetDirectory(dataDirectory, previousVersion)
    await mkdir(oldDirectory, { recursive: true })
    await createOwnedPreviousInstall(oldDirectory)
    const oldCli = await readFile(join(oldDirectory, 'cli/device-cli.mjs'), 'utf8')
    const marketplaces = client === 'codex'
      ? [{ name: 'esp-vision', root: oldDirectory, marketplaceSource: { sourceType: 'local', source: oldDirectory } }]
      : [{ name: 'esp-vision', installLocation: join(testRoot, 'claude cached marketplace'), source: 'local', repo: oldDirectory }]
    const plugins = client === 'codex'
      ? [{ name: 'esp-vision', marketplaceName: 'esp-vision', pluginId: 'esp-vision@esp-vision', enabled: true, version: previousVersion }]
      : [{ id: 'esp-vision@esp-vision', installPath: oldDirectory, enabled: true, scope: 'user', version: previousVersion }]
    const runner = createRunner({ client, marketplaces, plugins })
    const result = await installPlugin({ client, dataDirectory, packageRoot, commandRunner: runner })
    assert.equal(result.copied, true)
    assert.equal(result.marketplaceState, 'completed')
    assert.equal(result.pluginState, 'completed')
    const newDirectory = await targetDirectory(dataDirectory, packageVersion)
    assert.equal(result.installDirectory, newDirectory)
    assert.match(await readFile(join(newDirectory, 'cli/device-cli.mjs'), 'utf8'), /^#!\/usr\/bin\/env node/)
    assert.deepEqual(await readFile(join(oldDirectory, 'cli/device-cli.mjs'), 'utf8'), oldCli)
    assert.equal(JSON.parse(await readFile(join(oldDirectory, '.esp-vision-install.json'), 'utf8')).version, previousVersion)
    assert.equal(JSON.parse(await readFile(join(newDirectory, '.esp-vision-install.json'), 'utf8')).version, packageVersion)
    if (client === 'codex') {
      assert.deepEqual(mutations(runner.calls), [
        { command: 'codex', args: ['plugin', 'remove', 'esp-vision@esp-vision'] },
        { command: 'codex', args: ['plugin', 'marketplace', 'remove', 'esp-vision'] },
        { command: 'codex', args: ['plugin', 'marketplace', 'add', newDirectory] },
        { command: 'codex', args: ['plugin', 'add', 'esp-vision@esp-vision'] },
      ])
    } else {
      assert.deepEqual(mutations(runner.calls), [
        { command: 'claude', args: ['plugin', 'uninstall', '--keep-data', '--scope', 'user', 'esp-vision@esp-vision'] },
        { command: 'claude', args: ['plugin', 'marketplace', 'remove', 'esp-vision'] },
        { command: 'claude', args: ['plugin', 'marketplace', 'add', newDirectory] },
        { command: 'claude', args: ['plugin', 'install', 'esp-vision@esp-vision', '--scope', 'user'] },
      ])
    }
    assert.equal(runner.state.plugins[0].version, packageVersion)
    assert.equal(runner.state.marketplaces[0].name, 'esp-vision')
  }
})

test('upgrade failures report inspected partial state and rerun resumes safely', async () => {
  const dataDirectory = join(testRoot, 'upgrade recovery data')
  const oldDirectory = await targetDirectory(dataDirectory, previousVersion)
  await mkdir(oldDirectory, { recursive: true })
  await createOwnedPreviousInstall(oldDirectory)
  const runner = createRunner({
    client: 'codex',
    failAt: 3,
    marketplaces: [{ name: 'esp-vision', root: oldDirectory, marketplaceSource: { sourceType: 'local', source: oldDirectory } }],
    plugins: [{ name: 'esp-vision', marketplaceName: 'esp-vision', pluginId: 'esp-vision@esp-vision', enabled: true, version: previousVersion }],
  })
  await assert.rejects(installPlugin({ client: 'codex', dataDirectory, packageRoot, commandRunner: runner }), error => {
    assert.equal(error.code, 'REGISTRATION_FAILED')
    assert.equal(error.state.files, 'copied')
    assert.equal(error.state.previousVersion, previousVersion)
    assert.equal(error.state.previousFiles, 'preserved')
    assert.equal(error.state.marketplace, 'unregistered')
    assert.equal(error.state.plugin, 'not-installed')
    assert.equal(error.state.failedOperation, 'marketplace-add')
    assert.match(error.message, /Verified native state: marketplace unregistered; plugin not-installed/)
    assert.match(error.message, /rerun this installer/)
    assert.doesNotMatch(error.message, /private child stderr/)
    return true
  })
  assert.equal(JSON.parse(await readFile(join(oldDirectory, '.esp-vision-install.json'), 'utf8')).version, previousVersion)
  assert.equal((await lstat(await targetDirectory(dataDirectory, packageVersion))).isDirectory(), true)

  const resumed = await installPlugin({ client: 'codex', dataDirectory, packageRoot, commandRunner: runner })
  assert.equal(resumed.copied, false)
  assert.equal(resumed.marketplaceState, 'completed')
  assert.equal(resumed.pluginState, 'completed')
  assert.equal(runner.state.plugins[0].version, packageVersion)
})

test('refuses to upgrade a modified previous bundle before changing native state', async () => {
  const dataDirectory = join(testRoot, 'modified previous data')
  const oldDirectory = await targetDirectory(dataDirectory, previousVersion)
  await mkdir(oldDirectory, { recursive: true })
  await createOwnedPreviousInstall(oldDirectory)
  await writeFile(join(oldDirectory, 'cli/device-cli.mjs'), '# modified after install\n')
  const runner = createRunner({
    client: 'codex',
    marketplaces: [{ name: 'esp-vision', root: oldDirectory, marketplaceSource: { sourceType: 'local', source: oldDirectory } }],
    plugins: [{ pluginId: 'esp-vision@esp-vision', name: 'esp-vision', marketplaceName: 'esp-vision', enabled: true, version: previousVersion }],
  })

  await assert.rejects(installPlugin({ client: 'codex', dataDirectory, packageRoot, commandRunner: runner }), error => {
    assert.equal(error.code, 'FOREIGN_MARKETPLACE')
    assert.match(error.message, /not a valid installer-owned version/)
    return true
  })
  assert.deepEqual(mutations(runner.calls), [])
  assert.equal(JSON.parse(await readFile(join(oldDirectory, '.esp-vision-install.json'), 'utf8')).version, previousVersion)
})

test('rejects a foreign same-name marketplace and disabled plugin without mutating client state', async () => {
  const foreignDataDirectory = join(testRoot, 'foreign marketplace data')
  const foreignMarket = join(testRoot, 'foreign marketplace')
  await mkdir(foreignMarket, { recursive: true })
  const foreignRunner = createRunner({
    client: 'claude',
    marketplaces: [{ name: 'esp-vision', source: 'github', installLocation: foreignMarket }],
  })
  await assert.rejects(installPlugin({
    client: 'claude', dataDirectory: foreignDataDirectory, packageRoot, commandRunner: foreignRunner,
  }), error => error.code === 'FOREIGN_MARKETPLACE' && error.message.includes('outside this installer data directory'))
  assert.deepEqual(mutations(foreignRunner.calls), [])
  await assert.rejects(lstat(foreignDataDirectory), error => error.code === 'ENOENT')

  for (const client of ['codex', 'claude']) {
    const dataDirectory = join(testRoot, `${client} disabled data`)
    const runner = createRunner({ client })
    await installPlugin({ client, dataDirectory, packageRoot, commandRunner: runner })
    runner.state.plugins[0].enabled = false
    const mutationCount = mutations(runner.calls).length
    await assert.rejects(installPlugin({ client, dataDirectory, packageRoot, commandRunner: runner }), error => {
      assert.equal(error.code, 'PLUGIN_DISABLED')
      assert.match(error.message, /installed but disabled/)
      if (client === 'codex') assert.match(error.message, /Codex plugin settings/)
      else assert.match(error.message, /claude plugin enable/)
      return true
    })
    assert.equal(mutations(runner.calls).length, mutationCount)
  }
})

test('copy failure removes staging data, runs no client commands and preserves older versions', async () => {
  const dataDirectory = join(testRoot, 'copy failure data')
  const oldVersion = await targetDirectory(dataDirectory, previousVersion)
  await mkdir(oldVersion, { recursive: true })
  await writeFile(join(oldVersion, 'preserved.txt'), 'previous version')
  const calls = []
  let count = 0

  await assert.rejects(installPlugin({
    client: 'codex',
    dataDirectory,
    packageRoot,
    commandRunner: createRunner({ calls }),
    copyFileImpl: async (...args) => {
      count += 1
      if (count === 2) throw new Error('private copy failure')
      return copyFile(...args)
    },
  }), error => error.code === 'COPY_FAILED'
    && error.state.files === 'not-installed'
    && error.state.marketplace === 'not-attempted'
    && error.message.includes('existing versioned installs were left unchanged')
    && !error.message.includes('private copy failure'))

  assert.equal(await readFile(join(oldVersion, 'preserved.txt'), 'utf8'), 'previous version')
  assert.deepEqual(await readdir(join(dataDirectory, 'plugins')), [previousVersion])
  assert.deepEqual(mutations(calls), [])
})

test('registration failures report inspected native-client state and a rerun path', async () => {
  for (const failureIndex of [1, 2]) {
    const dataDirectory = join(testRoot, `registration failure ${failureIndex}`)
    const calls = []
    await assert.rejects(installPlugin({
      client: 'codex', dataDirectory, packageRoot, commandRunner: createRunner({ calls, failAt: failureIndex }),
    }), error => {
      assert.equal(error.code, 'REGISTRATION_FAILED')
      assert.equal(error.state.files, 'copied')
      assert.equal(error.state.marketplace, failureIndex === 1 ? 'unregistered' : 'registered-current')
      assert.equal(error.state.plugin, 'not-installed')
      assert.match(error.message, /Verified native state/)
      assert.match(error.message, /rerun this installer/)
      assert.match(error.message, /No host configuration files were edited directly/)
      assert.doesNotMatch(error.message, /private child stderr/)
      return true
    })
    assert.equal(mutations(calls).length, failureIndex)
    await lstat(await targetDirectory(dataDirectory))
  }
})

test('rejects symlink and unrecognized target directories without running commands', async () => {
  const dataDirectory = join(testRoot, 'unsafe target data')
  const pluginsDirectory = join(dataDirectory, 'plugins')
  await mkdir(pluginsDirectory, { recursive: true })
  const outsideTarget = join(testRoot, 'outside target')
  await mkdir(outsideTarget)
  const symlinkTarget = await targetDirectory(dataDirectory)
  await symlink(outsideTarget, symlinkTarget, 'dir')
  const calls = []
  await assert.rejects(installPlugin({
    client: 'codex', dataDirectory, packageRoot, commandRunner: createRunner({ calls }),
  }), error => error.code === 'UNSAFE_DIRECTORY')
  assert.deepEqual(mutations(calls), [])
  await rm(symlinkTarget)

  const foreignTarget = await targetDirectory(dataDirectory)
  await mkdir(foreignTarget)
  await writeFile(join(foreignTarget, 'owned-by-someone-else.txt'), 'keep')
  await assert.rejects(installPlugin({
    client: 'codex', dataDirectory, packageRoot, commandRunner: createRunner({ calls }),
  }), error => error.code === 'FOREIGN_INSTALL')
  assert.equal(await readFile(join(foreignTarget, 'owned-by-someone-else.txt'), 'utf8'), 'keep')
  assert.deepEqual(mutations(calls), [])
})

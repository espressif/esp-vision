// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { execFile as execFileCallback } from 'node:child_process'
import { constants } from 'node:fs'
import {
  chmod,
  copyFile,
  lstat,
  mkdir,
  readFile,
  readdir,
  rename,
  rm,
  writeFile,
} from 'node:fs/promises'
import { createHash, randomUUID } from 'node:crypto'
import { homedir } from 'node:os'
import { dirname, isAbsolute, join, parse, relative, resolve, sep, win32 as win32Path } from 'node:path'
import { fileURLToPath } from 'node:url'
import { promisify } from 'node:util'
import { deflateRawSync } from 'node:zlib'

const execFile = promisify(execFileCallback)
const PACKAGE_NAME = '@esp-vision/plugin'
const PLUGIN_NAME = 'esp-vision'
const INSTALL_MARKER = '.esp-vision-install.json'
const WORKBUDDY_EXPORT_MARKER = '.esp-vision-workbuddy-export.json'
const WORKBUDDY_SKILL_NAME = 'esp-vision-development'
const WORKBUDDY_EXPORT_DIRECTORY = 'workbuddy'
const PACKAGE_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const PAYLOAD_ENTRIES = Object.freeze([
  '.agents/plugins/marketplace.json',
  '.claude-plugin/marketplace.json',
  '.claude-plugin/plugin.json',
  '.mcp.json',
  'assets',
  'DEVICE_CORE_NOTICE.md',
  'LICENSE',
  'README.md',
  'THIRD_PARTY_NOTICES.md',
  'mcp.json',
  'plugin.json',
  'cli',
  'skills',
  'workbuddy',
])
const REQUIRED_PACKAGE_FILES = Object.freeze([
  'bin/esp-vision-plugin.mjs',
  'src/installer.mjs',
  ...PAYLOAD_ENTRIES.filter(entry => entry !== 'skills'),
  'skills',
])
const SEMVER_PATTERN = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/

export class InstallerError extends Error {
  constructor(message, { code = 'INSTALL_FAILED', state = {}, cause } = {}) {
    super(message, cause ? { cause } : undefined)
    this.name = 'InstallerError'
    this.code = code
    this.state = Object.freeze({ ...state })
  }
}

function fail(message, code, cause, state = {}) {
  throw new InstallerError(message, { code, cause, state })
}

function expectObject(value, description) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    fail(`The packaged ${description} is invalid.`, 'INVALID_BUNDLE')
  }
  return value
}

async function readJson(path, description) {
  try {
    return JSON.parse(await readFile(path, 'utf8'))
  } catch (error) {
    if (error instanceof InstallerError) throw error
    fail(`The packaged ${description} is missing or invalid.`, 'INVALID_BUNDLE', error)
  }
}

function assertEqual(actual, expected, description) {
  if (actual !== expected) {
    fail(`The packaged ${description} is invalid.`, 'INVALID_BUNDLE')
  }
}

async function collectRegularFiles(root, entry, output) {
  const absolute = resolve(root, entry)
  const info = await lstat(absolute).catch(error => {
    fail(`Required package asset is missing: ${entry}`, 'MISSING_ASSET', error)
  })
  if (info.isSymbolicLink()) fail(`Package asset must not be a symlink: ${entry}`, 'INVALID_BUNDLE')
  if (info.isFile()) {
    if (info.size === 0) fail(`Package asset is empty: ${entry}`, 'INVALID_BUNDLE')
    output.push({ path: entry, mode: info.mode & 0o777 })
    return
  }
  if (!info.isDirectory()) fail(`Package asset has an unsupported file type: ${entry}`, 'INVALID_BUNDLE')

  const children = (await readdir(absolute, { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name))
  for (const child of children) {
    const childPath = `${entry}/${child.name}`
    if (child.isSymbolicLink()) fail(`Package asset must not be a symlink: ${childPath}`, 'INVALID_BUNDLE')
    if (child.isDirectory()) {
      await collectRegularFiles(root, childPath, output)
    } else if (child.isFile()) {
      const childInfo = await lstat(resolve(root, childPath))
      if (!childInfo.isFile() || childInfo.isSymbolicLink()) fail(`Package asset is not a regular file: ${childPath}`, 'INVALID_BUNDLE')
      if (childInfo.size === 0) fail(`Package asset is empty: ${childPath}`, 'INVALID_BUNDLE')
      output.push({ path: childPath, mode: childInfo.mode & 0o777 })
    } else {
      fail(`Package asset has an unsupported file type: ${childPath}`, 'INVALID_BUNDLE')
    }
  }
}

async function collectPayload(root) {
  const files = []
  for (const entry of PAYLOAD_ENTRIES) await collectRegularFiles(root, entry, files)
  files.sort((a, b) => a.path.localeCompare(b.path))
  for (const required of [
    'assets/esp-vision-logo.png',
    'cli/device-cli.mjs',
    'cli/node_serial.py',
    'cli/node_rom.py',
    'skills/esp-vision-development/SKILL.md',
    'skills/esp-vision-development/references/application-workflow.md',
    'workbuddy/mcp.json',
    'workbuddy/README.md',
  ]) {
    if (!files.some(file => file.path === required)) fail(`Required package asset is missing: ${required}`, 'MISSING_ASSET')
  }
  if (!files.some(file => file.path.startsWith('cli/licenses/'))) {
    fail('The bundled CLI third-party license directory is empty.', 'MISSING_ASSET')
  }
  return files
}

async function calculateBundleHash(root, files) {
  const hash = createHash('sha256')
  for (const file of files) {
    hash.update(file.path)
    hash.update('\0')
    hash.update(await readFile(resolve(root, file.path)))
    hash.update('\0')
  }
  return hash.digest('hex')
}

function validateCodexCatalog(catalog) {
  expectObject(catalog, 'Codex marketplace catalog')
  assertEqual(catalog.name, PLUGIN_NAME, 'Codex marketplace name')
  if (!Array.isArray(catalog.plugins) || catalog.plugins.length !== 1) {
    fail('The packaged Codex marketplace catalog is invalid.', 'INVALID_BUNDLE')
  }
  const plugin = expectObject(catalog.plugins[0], 'Codex marketplace plugin entry')
  assertEqual(plugin.name, PLUGIN_NAME, 'Codex marketplace plugin name')
  const source = expectObject(plugin.source, 'Codex marketplace source')
  assertEqual(source.source, 'local', 'Codex marketplace source type')
  assertEqual(source.path, './', 'Codex marketplace plugin path')
}

function validateClaudeCatalog(catalog) {
  expectObject(catalog, 'Claude marketplace catalog')
  assertEqual(catalog.name, PLUGIN_NAME, 'Claude marketplace name')
  if (!Array.isArray(catalog.plugins) || catalog.plugins.length !== 1) {
    fail('The packaged Claude marketplace catalog is invalid.', 'INVALID_BUNDLE')
  }
  const plugin = expectObject(catalog.plugins[0], 'Claude marketplace plugin entry')
  assertEqual(plugin.name, PLUGIN_NAME, 'Claude marketplace plugin name')
  assertEqual(plugin.source, './', 'Claude marketplace plugin path')
}

async function validatePayload(root, packageVersion) {
  const files = await collectPayload(root)
  const npmManifestInfo = await lstat(resolve(root, 'package.json')).catch(error => {
    fail('Required package asset is missing: package.json', 'MISSING_ASSET', error)
  })
  if (!npmManifestInfo.isFile() || npmManifestInfo.isSymbolicLink()) {
    fail('The packaged npm manifest must be a regular file.', 'INVALID_BUNDLE')
  }
  const packageManifest = expectObject(await readJson(resolve(root, 'package.json'), 'npm package manifest'), 'npm package manifest')
  assertEqual(packageManifest.name, PACKAGE_NAME, 'npm package name')
  assertEqual(packageManifest.version, packageVersion, 'npm package version')

  const pluginManifest = expectObject(await readJson(resolve(root, 'plugin.json'), 'Codex plugin manifest'), 'Codex plugin manifest')
  assertEqual(pluginManifest.name, PLUGIN_NAME, 'Codex plugin name')
  assertEqual(pluginManifest.version, packageVersion, 'Codex plugin version')
  const claudeManifest = expectObject(await readJson(resolve(root, '.claude-plugin/plugin.json'), 'Claude plugin manifest'), 'Claude plugin manifest')
  assertEqual(claudeManifest.name, PLUGIN_NAME, 'Claude plugin name')
  assertEqual(claudeManifest.version, packageVersion, 'Claude plugin version')

  validateCodexCatalog(await readJson(resolve(root, '.agents/plugins/marketplace.json'), 'Codex marketplace catalog'))
  validateClaudeCatalog(await readJson(resolve(root, '.claude-plugin/marketplace.json'), 'Claude marketplace catalog'))

  const codexMcp = expectObject(await readJson(resolve(root, 'mcp.json'), 'Codex MCP configuration'), 'Codex MCP configuration')
  const codexServers = expectObject(codexMcp.mcpServers, 'Codex MCP server list')
  if (Object.keys(codexServers).length !== 1) fail('The packaged Codex MCP configuration must contain only official ESP-VISION Knowledge MCP.', 'INVALID_BUNDLE')
  const codexKnowledge = expectObject(codexServers['esp-vision-knowledge'], 'Codex Knowledge MCP entry')
  assertEqual(codexKnowledge.type, 'streamable-http', 'Codex Knowledge MCP transport')
  assertEqual(codexKnowledge.url, 'https://mcp.vision.espressif.com/mcp', 'Codex Knowledge MCP URL')
  const claudeMcp = expectObject(await readJson(resolve(root, '.mcp.json'), 'Claude MCP configuration'), 'Claude MCP configuration')
  const claudeServers = expectObject(claudeMcp.mcpServers, 'Claude MCP server list')
  if (Object.keys(claudeServers).length !== 1) fail('The packaged Claude MCP configuration must contain only official ESP-VISION Knowledge MCP.', 'INVALID_BUNDLE')
  const claudeKnowledge = expectObject(claudeServers['esp-vision-knowledge'], 'Claude Knowledge MCP entry')
  assertEqual(claudeKnowledge.type, 'http', 'Claude Knowledge MCP transport')
  assertEqual(claudeKnowledge.url, 'https://mcp.vision.espressif.com/mcp', 'Claude Knowledge MCP URL')

  const workbuddyMcp = expectObject(await readJson(resolve(root, 'workbuddy/mcp.json'), 'WorkBuddy MCP configuration'), 'WorkBuddy MCP configuration')
  const workbuddyServers = expectObject(workbuddyMcp.mcpServers, 'WorkBuddy MCP server list')
  if (Object.keys(workbuddyServers).length !== 1) fail('The WorkBuddy MCP configuration must contain only official ESP-VISION Knowledge MCP.', 'INVALID_BUNDLE')
  const workbuddyKnowledge = expectObject(workbuddyServers['esp-vision-knowledge'], 'WorkBuddy Knowledge MCP entry')
  assertEqual(workbuddyKnowledge.type, 'streamableHttp', 'WorkBuddy Knowledge MCP transport')
  assertEqual(workbuddyKnowledge.url, 'https://mcp.vision.espressif.com/mcp', 'WorkBuddy Knowledge MCP URL')

  return { files, bundleHash: await calculateBundleHash(root, files) }
}

/** Validate the npm package and dependency-closed runtime before copying or registration. */
export async function validatePackage(packageRoot = PACKAGE_ROOT) {
  const root = resolve(packageRoot)
  const packageManifest = expectObject(await readJson(resolve(root, 'package.json'), 'npm package manifest'), 'npm package manifest')
  assertEqual(packageManifest.name, PACKAGE_NAME, 'npm package name')
  if (typeof packageManifest.version !== 'string' || !SEMVER_PATTERN.test(packageManifest.version)) {
    fail('The packaged npm version is invalid.', 'INVALID_BUNDLE')
  }
  if (packageManifest.dependencies && Object.keys(packageManifest.dependencies).length > 0) {
    fail('The installer npm package must not have runtime dependencies.', 'INVALID_BUNDLE')
  }
  if (!Array.isArray(packageManifest.files) || REQUIRED_PACKAGE_FILES.some(file => !packageManifest.files.includes(file))) {
    fail('The npm package files allowlist does not include the installer and complete plugin bundle.', 'INVALID_BUNDLE')
  }
  for (const required of ['bin/esp-vision-plugin.mjs', 'src/installer.mjs']) {
    const fileInfo = await lstat(resolve(root, required)).catch(error => {
      fail(`Required installer asset is missing: ${required}`, 'MISSING_ASSET', error)
    })
    if (!fileInfo.isFile() || fileInfo.isSymbolicLink()) fail(`Installer asset is not a regular file: ${required}`, 'INVALID_BUNDLE')
    if (fileInfo.size === 0) fail(`Installer asset is empty: ${required}`, 'INVALID_BUNDLE')
  }
  const bundle = await validatePayload(root, packageManifest.version)
  return { packageRoot: root, version: packageManifest.version, ...bundle }
}

const ZIP_CRC_TABLE = (() => {
  const table = new Uint32Array(256)
  for (let index = 0; index < table.length; index += 1) {
    let value = index
    for (let bit = 0; bit < 8; bit += 1) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1
    table[index] = value >>> 0
  }
  return table
})()

function zipCrc32(buffer) {
  let crc = 0xffffffff
  for (const byte of buffer) crc = ZIP_CRC_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8)
  return (crc ^ 0xffffffff) >>> 0
}

export function createZip(entries) {
  const localRecords = []
  const centralRecords = []
  let offset = 0
  for (const entry of entries) {
    const name = Buffer.from(entry.name.replaceAll('\\', '/'), 'utf8')
    const contents = Buffer.isBuffer(entry.contents) ? entry.contents : Buffer.from(entry.contents)
    const compressed = deflateRawSync(contents, { level: 9 })
    const crc = zipCrc32(contents)
    if (name.length > 0xffff || contents.length > 0xffffffff || compressed.length > 0xffffffff) {
      fail('The WorkBuddy Skill package contains an unsupported file size.', 'WORKBUDDY_EXPORT_FAILED')
    }

    const local = Buffer.alloc(30 + name.length)
    local.writeUInt32LE(0x04034b50, 0)
    local.writeUInt16LE(20, 4)
    local.writeUInt16LE(0x0800, 6)
    local.writeUInt16LE(8, 8)
    local.writeUInt16LE(0, 10)
    local.writeUInt16LE(0x0021, 12)
    local.writeUInt32LE(crc, 14)
    local.writeUInt32LE(compressed.length, 18)
    local.writeUInt32LE(contents.length, 22)
    local.writeUInt16LE(name.length, 26)
    local.writeUInt16LE(0, 28)
    name.copy(local, 30)
    localRecords.push(local, compressed)

    const central = Buffer.alloc(46 + name.length)
    central.writeUInt32LE(0x02014b50, 0)
    central.writeUInt16LE(0x0314, 4)
    central.writeUInt16LE(20, 6)
    central.writeUInt16LE(0x0800, 8)
    central.writeUInt16LE(8, 10)
    central.writeUInt16LE(0, 12)
    central.writeUInt16LE(0x0021, 14)
    central.writeUInt32LE(crc, 16)
    central.writeUInt32LE(compressed.length, 20)
    central.writeUInt32LE(contents.length, 24)
    central.writeUInt16LE(name.length, 28)
    central.writeUInt16LE(0, 30)
    central.writeUInt16LE(0, 32)
    central.writeUInt16LE(0, 34)
    central.writeUInt16LE(0, 36)
    central.writeUInt32LE(((0o100000 | (entry.mode ?? 0o644)) << 16) >>> 0, 38)
    central.writeUInt32LE(offset, 42)
    name.copy(central, 46)
    centralRecords.push(central)
    offset += local.length + compressed.length
  }
  const centralDirectory = Buffer.concat(centralRecords)
  const end = Buffer.alloc(22)
  end.writeUInt32LE(0x06054b50, 0)
  end.writeUInt16LE(0, 4)
  end.writeUInt16LE(0, 6)
  end.writeUInt16LE(entries.length, 8)
  end.writeUInt16LE(entries.length, 10)
  end.writeUInt32LE(centralDirectory.length, 12)
  end.writeUInt32LE(offset, 16)
  end.writeUInt16LE(0, 20)
  return Buffer.concat([...localRecords, centralDirectory, end])
}

function hashWorkbuddyArtifacts(artifacts) {
  const hash = createHash('sha256')
  for (const [name, contents] of [...artifacts].sort(([left], [right]) => left.localeCompare(right))) {
    hash.update(name)
    hash.update('\0')
    hash.update(contents)
    hash.update('\0')
  }
  return hash.digest('hex')
}

async function makeWorkbuddyArtifacts(packageInfo, installDirectory) {
  const commonSkill = await readFile(resolve(packageInfo.packageRoot, 'skills/esp-vision-development/SKILL.md'), 'utf8')
  const oldCliParagraph = 'Invoke the installed plugin copy with Node and the absolute path to `cli/device-cli.mjs`. Resolve the plugin root from the current host\'s installed plugin path, not the user\'s project working directory. Codex hook commands can use `PLUGIN_ROOT`; Claude Code hook commands can use `CLAUDE_PLUGIN_ROOT`. If a shell session does not expose either path, read the local marketplace/plugin path with the host\'s native list command and use the installed copy. Do not invoke a global `esp-vision-device` command, `npx`, or a separate MCP device server. The standalone `@esp-vision/device-cli` package is also available to users who want the CLI outside this plugin; the plugin carries its own versioned copy.'
  const portableCliPath = join(installDirectory, 'cli/device-cli.mjs').replaceAll('\\', '/')
  const workbuddyCliParagraph = `Invoke Node with this persistent local CLI path: \`${portableCliPath}\`. Quote the path because it may contain spaces. This path is independent of WorkBuddy's internal Skill storage. The ZIP also includes a dependency-closed portable CLI copy at \`scripts/cli/device-cli.mjs\`. Do not invoke a global \`esp-vision-device\` command, \`npx\`, or a separate MCP device server.`
  if (!commonSkill.includes(oldCliParagraph)) fail('The packaged ESP-VISION Skill no longer matches the WorkBuddy adapter template.', 'INVALID_BUNDLE')
  let skill = commonSkill
    .replace(oldCliParagraph, workbuddyCliParagraph)
    .replace('Use `node <plugin-root>/cli/device-cli.mjs --help` for command syntax.', `Use \`node "${portableCliPath}" --help\` for command syntax.`)
    .replaceAll('[application workflow](references/application-workflow.md)', '@references/application-workflow.md')
  skill = skill.replace(/^description: (.+)$/m, 'description: $1\ndescription_en: $1\ndescription_zh: 使用 ESP-VISION 设备能力与官方固件/API 知识完成具体任务。\nversion: ' + packageInfo.version + '\nauthor: Espressif Systems')
  if (skill.includes('PLUGIN_ROOT') || skill.includes('CLAUDE_PLUGIN_ROOT') || skill.includes('<plugin-root>') || skill.includes('[application workflow]')) {
    fail('The WorkBuddy Skill still contains host-specific plugin paths.', 'INVALID_BUNDLE')
  }

  const zipEntries = [{
    name: `${WORKBUDDY_SKILL_NAME}/SKILL.md`, contents: Buffer.from(skill), mode: 0o644,
  }]
  const referencePath = resolve(packageInfo.packageRoot, 'skills/esp-vision-development/references/application-workflow.md')
  zipEntries.push({
    name: `${WORKBUDDY_SKILL_NAME}/references/application-workflow.md`,
    contents: await readFile(referencePath),
    mode: 0o644,
  })
  for (const file of packageInfo.files.filter(item => item.path.startsWith('cli/'))) {
    zipEntries.push({
      name: `${WORKBUDDY_SKILL_NAME}/scripts/${file.path}`,
      contents: await readFile(resolve(packageInfo.packageRoot, file.path)),
      mode: file.mode,
    })
  }
  for (const file of ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'DEVICE_CORE_NOTICE.md']) {
    let contents = await readFile(resolve(packageInfo.packageRoot, file))
    if (file === 'THIRD_PARTY_NOTICES.md') {
      contents = Buffer.from(contents.toString('utf8').replaceAll('`cli/licenses/`', '`scripts/cli/licenses/`'))
    }
    zipEntries.push({
      name: `${WORKBUDDY_SKILL_NAME}/${file}`,
      contents,
      mode: 0o644,
    })
  }
  for (const file of ['assets/esp-vision-logo.png', 'assets/PROVENANCE.md']) {
    zipEntries.push({
      name: `${WORKBUDDY_SKILL_NAME}/${file}`,
      contents: await readFile(resolve(packageInfo.packageRoot, file)),
      mode: 0o644,
    })
  }
  const zip = createZip(zipEntries.sort((left, right) => left.name.localeCompare(right.name)))
  const workbuddyMcp = await readFile(resolve(packageInfo.packageRoot, 'workbuddy/mcp.json'))
  const installGuideTemplate = await readFile(resolve(packageInfo.packageRoot, 'workbuddy/README.md'), 'utf8')
  const installGuide = installGuideTemplate.replaceAll('{{CLI_PATH}}', portableCliPath)
  if (installGuide.includes('{{')) fail('The WorkBuddy installation guide template is invalid.', 'INVALID_BUNDLE')
  const artifacts = new Map([
    ['esp-vision-development.zip', zip],
    ['mcp.json', workbuddyMcp],
    ['INSTALL.md', Buffer.from(installGuide)],
  ])
  return { artifacts, artifactHash: hashWorkbuddyArtifacts(artifacts) }
}

async function inspectWorkbuddyExport(directory, version, expectedHash) {
  let info
  try {
    info = await lstat(directory)
  } catch (error) {
    if (error.code === 'ENOENT') return { exists: false }
    throw error
  }
  if (info.isSymbolicLink()) fail(`Refusing to replace a symlink at the WorkBuddy export path: ${directory}`, 'UNSAFE_DIRECTORY')
  if (!info.isDirectory()) fail(`Refusing to replace a foreign file at the WorkBuddy export path: ${directory}`, 'FOREIGN_EXPORT')

  const markerPath = join(directory, WORKBUDDY_EXPORT_MARKER)
  let markerInfo
  try {
    markerInfo = await lstat(markerPath)
  } catch (error) {
    if (error.code === 'ENOENT') fail(`Refusing to replace an unrecognized WorkBuddy export directory: ${directory}`, 'FOREIGN_EXPORT')
    throw error
  }
  if (!markerInfo.isFile() || markerInfo.isSymbolicLink()) fail('The WorkBuddy export marker is invalid.', 'FOREIGN_EXPORT')
  let marker
  try {
    marker = JSON.parse(await readFile(markerPath, 'utf8'))
  } catch (error) {
    fail('The WorkBuddy export marker is invalid.', 'FOREIGN_EXPORT', error)
  }
  if (marker?.name !== PACKAGE_NAME || marker?.version !== version || !/^[a-f0-9]{64}$/.test(marker?.artifactHash ?? '')) {
    fail(`Refusing to replace a foreign WorkBuddy export directory: ${directory}`, 'FOREIGN_EXPORT')
  }

  const names = (await readdir(directory)).sort()
  const expectedNames = ['INSTALL.md', 'esp-vision-development.zip', 'mcp.json', WORKBUDDY_EXPORT_MARKER].sort()
  if (names.length !== expectedNames.length || names.some((name, index) => name !== expectedNames[index])) {
    fail(`The WorkBuddy export at ${directory} contains modified or extra files; it was left unchanged. Remove that export directory to recover.`, 'WORKBUDDY_EXPORT_MODIFIED')
  }
  const artifacts = new Map()
  for (const name of expectedNames.filter(value => value !== WORKBUDDY_EXPORT_MARKER)) {
    const artifactInfo = await lstat(join(directory, name))
    if (!artifactInfo.isFile() || artifactInfo.isSymbolicLink()) fail('A WorkBuddy export artifact is not a regular file.', 'WORKBUDDY_EXPORT_MODIFIED')
    artifacts.set(name, await readFile(join(directory, name)))
  }
  const actualHash = hashWorkbuddyArtifacts(artifacts)
  if (actualHash !== marker.artifactHash || actualHash !== expectedHash) {
    fail(`The WorkBuddy export at ${directory} differs from this verified package; it was left unchanged. Remove that export directory to recover.`, 'WORKBUDDY_EXPORT_MODIFIED')
  }
  return { exists: true }
}

export function defaultDataDirectory({ platform = process.platform, env = process.env, home = homedir() } = {}) {
  if (platform === 'win32') {
    const base = env.LOCALAPPDATA || win32Path.join(home, 'AppData', 'Local')
    return win32Path.join(base, 'Espressif', 'ESP-VISION')
  }
  if (platform === 'darwin') return join(home, 'Library', 'Application Support', 'Espressif', 'ESP-VISION')
  const xdg = env.XDG_DATA_HOME
  const base = xdg && isAbsolute(xdg) ? xdg : join(home, '.local', 'share')
  return join(base, 'esp-vision')
}

export function parseInstallArgs(argv) {
  const args = [...argv]
  if (args.length === 1 && ['--help', '-h'].includes(args[0])) return { help: true }
  if (args[0] !== 'install') fail('Usage: esp-vision-plugin install <codex|claude|workbuddy> [--dry-run] [--data-dir DIR]', 'INVALID_ARGUMENTS')
  const client = args[1]
  if (!['codex', 'claude', 'workbuddy'].includes(client)) fail('Choose `codex`, `claude`, or `workbuddy` as the install target.', 'INVALID_ARGUMENTS')
  let dryRun = false
  let dataDirectory
  for (let index = 2; index < args.length; index += 1) {
    const arg = args[index]
    if (arg === '--dry-run') {
      if (dryRun) fail('The `--dry-run` option may only be used once.', 'INVALID_ARGUMENTS')
      dryRun = true
      continue
    }
    if (arg === '--data-dir') {
      if (dataDirectory !== undefined) fail('The `--data-dir` option may only be used once.', 'INVALID_ARGUMENTS')
      const value = args[index + 1]
      if (typeof value !== 'string' || value.length === 0 || value.startsWith('--')) {
        fail('The `--data-dir` option requires a directory path.', 'INVALID_ARGUMENTS')
      }
      dataDirectory = value
      index += 1
      continue
    }
    fail(`Unknown install option: ${arg}`, 'INVALID_ARGUMENTS')
  }
  return { client, dryRun, ...(dataDirectory !== undefined ? { dataDirectory } : {}) }
}

function clientCommands(client, installDirectory) {
  if (client === 'codex') {
    return [
      { command: 'codex', args: ['plugin', 'marketplace', 'add', installDirectory] },
      { command: 'codex', args: ['plugin', 'add', `${PLUGIN_NAME}@${PLUGIN_NAME}`] },
    ]
  }
  if (client === 'claude') {
    return [
      { command: 'claude', args: ['plugin', 'marketplace', 'add', installDirectory] },
      { command: 'claude', args: ['plugin', 'install', `${PLUGIN_NAME}@${PLUGIN_NAME}`] },
    ]
  }
  fail('Choose `codex` or `claude` as the install target.', 'INVALID_ARGUMENTS')
}

function upgradeCommands(client, installDirectory, installedPlugin) {
  const commands = []
  if (installedPlugin) {
    if (client === 'codex') {
      commands.push({ kind: 'plugin-remove', command: 'codex', args: ['plugin', 'remove', `${PLUGIN_NAME}@${PLUGIN_NAME}`] })
    } else {
      const scope = installedPlugin.scope
      if (!['user', 'project', 'local'].includes(scope)) {
        fail('Claude Code did not report a supported installation scope for the installed ESP-VISION plugin. Upgrade was not started; inspect `claude plugin list` and resolve the scope first.', 'CLIENT_STATE_UNAVAILABLE')
      }
      commands.push({
        kind: 'plugin-remove',
        command: 'claude',
        args: ['plugin', 'uninstall', '--keep-data', '--scope', scope, `${PLUGIN_NAME}@${PLUGIN_NAME}`],
      })
    }
  }
  commands.push({
    kind: 'marketplace-remove',
    command: client === 'codex' ? 'codex' : 'claude',
    args: ['plugin', 'marketplace', 'remove', PLUGIN_NAME],
  })
  commands.push({
    kind: 'marketplace-add',
    command: client === 'codex' ? 'codex' : 'claude',
    args: ['plugin', 'marketplace', 'add', installDirectory],
  })
  commands.push({
    kind: 'plugin-install',
    command: client === 'codex' ? 'codex' : 'claude',
    args: client === 'codex'
      ? ['plugin', 'add', `${PLUGIN_NAME}@${PLUGIN_NAME}`]
      : ['plugin', 'install', `${PLUGIN_NAME}@${PLUGIN_NAME}`, '--scope', installedPlugin?.scope ?? 'user'],
  })
  return commands
}

function stateQueryCommands(client) {
  if (client === 'codex') {
    return [
      ['plugin', 'marketplace', 'list', '--json'],
      ['plugin', 'list', '--json'],
    ]
  }
  return [
    ['plugin', 'marketplace', 'list', '--json'],
    ['plugin', 'list', '--json'],
  ]
}

function parseNativeJson(result, client, description) {
  try {
    const parsed = JSON.parse(String(result?.stdout ?? ''))
    if (!parsed || typeof parsed !== 'object') throw new Error('invalid JSON shape')
    if (client === 'codex' && !Array.isArray(parsed[description])) throw new Error('invalid JSON shape')
    if (client === 'claude' && !Array.isArray(parsed)) throw new Error('invalid JSON shape')
    return client === 'codex' ? parsed[description] : parsed
  } catch (error) {
    fail(`Could not read the ${client} ${description === 'marketplaces' ? 'marketplace' : 'plugin'} list. Run the native list command and resolve any client error before installing.`, 'CLIENT_STATE_UNAVAILABLE', error)
  }
}

async function inspectClientState(client, commandRunner) {
  const command = client === 'codex' ? 'codex' : 'claude'
  const [marketplaceArgs, pluginArgs] = stateQueryCommands(client)
  let marketplaces
  let plugins
  try {
    marketplaces = parseNativeJson(await commandRunner(command, marketplaceArgs), client, 'marketplaces')
    plugins = parseNativeJson(await commandRunner(command, pluginArgs), client, 'installed')
  } catch (error) {
    if (error instanceof InstallerError) throw error
    fail(`Could not inspect ${client} plugin registrations. Resolve the native client list command before installing.`, 'CLIENT_STATE_UNAVAILABLE', error)
  }
  return { marketplaces, plugins }
}

function marketplaceName(client, row) {
  if (client === 'codex') return typeof row?.name === 'string' ? row.name : undefined
  return typeof row?.name === 'string' ? row.name : undefined
}

function marketplacePath(client, row) {
  if (client === 'codex') return typeof row?.root === 'string' && row.root.length > 0 ? resolve(row.root) : undefined
  const source = row?.source
  const sourcePath = source && typeof source === 'object'
    ? [source.path, source.localPath, source.directory, source.sourcePath, source.source].find(value => typeof value === 'string' && isAbsolute(value))
    : undefined
  const localRepo = typeof row?.repo === 'string' && isAbsolute(row.repo) ? row.repo : undefined
  const value = sourcePath ?? localRepo ?? row?.installLocation
  return typeof value === 'string' && isAbsolute(value) ? resolve(value) : undefined
}

function isEspVisionPlugin(client, row) {
  if (client === 'codex') {
    return row?.pluginId === `${PLUGIN_NAME}@${PLUGIN_NAME}`
      || row?.id === `${PLUGIN_NAME}@${PLUGIN_NAME}`
      || (row?.name === PLUGIN_NAME && row?.marketplaceName === PLUGIN_NAME)
  }
  return row?.id === `${PLUGIN_NAME}@${PLUGIN_NAME}`
    || row?.pluginId === `${PLUGIN_NAME}@${PLUGIN_NAME}`
}

function pluginMarketplace(client, row) {
  if (client === 'codex') return row?.marketplaceName
  const id = row?.id ?? row?.pluginId
  return typeof id === 'string' && id.endsWith(`@${PLUGIN_NAME}`) ? PLUGIN_NAME : undefined
}

function summarizeClientState(client, state, currentPath, previousPath) {
  const marketplaces = state.marketplaces.filter(row => marketplaceName(client, row) === PLUGIN_NAME)
  let marketplace = 'unregistered'
  if (marketplaces.length > 1) marketplace = 'ambiguous'
  else if (marketplaces.length === 1) {
    const path = marketplacePath(client, marketplaces[0])
    marketplace = path === resolve(currentPath) ? 'registered-current'
      : previousPath && path === resolve(previousPath) ? 'registered-previous'
        : path ? 'registered-other-path' : 'registered-unknown-path'
  }
  const plugins = state.plugins.filter(row => isEspVisionPlugin(client, row))
  let plugin = 'not-installed'
  if (plugins.length > 1) plugin = 'ambiguous'
  else if (plugins.length === 1) {
    plugin = plugins[0].enabled === false ? 'installed-disabled'
      : pluginMarketplace(client, plugins[0]) !== PLUGIN_NAME ? 'installed-from-other-marketplace'
        : plugins[0].version === undefined ? 'installed-version-unknown'
          : plugins[0].version === state.expectedVersion ? 'installed-current'
            : previousPath ? 'installed-previous-or-other-version' : 'installed-other-version'
  }
  return { marketplace, plugin }
}

function verifyInstalledState(client, state, installDirectory, packageVersion) {
  const expected = summarizeClientState(client, { ...state, expectedVersion: packageVersion }, installDirectory)
  if (expected.marketplace !== 'registered-current' || expected.plugin !== 'installed-current') {
    fail(
      `The ${client} commands returned, but their JSON state does not confirm ESP-VISION ${packageVersion} at ${installDirectory} (marketplace: ${expected.marketplace}; plugin: ${expected.plugin}). Resolve the native client state and rerun the installer. The plugin files were preserved.`,
      'REGISTRATION_UNVERIFIED',
      undefined,
      { files: 'copied-or-preserved', ...expected },
    )
  }
}

function operationPlan(client, installDirectory, packageVersion, state) {
  const { marketplaces, plugins } = state
  const namedMarketplaces = marketplaces.filter(row => marketplaceName(client, row) === PLUGIN_NAME)
  if (namedMarketplaces.length > 1) {
    fail(`Multiple ${client} marketplaces are named ${PLUGIN_NAME}; resolve the duplicate registration before installing.`, 'AMBIGUOUS_MARKETPLACE')
  }
  const ownedPlugins = plugins.filter(row => isEspVisionPlugin(client, row))
  if (ownedPlugins.length > 1) {
    fail(`Multiple ${client} ${PLUGIN_NAME} plugin registrations exist; resolve the duplicate before installing.`, 'AMBIGUOUS_PLUGIN')
  }
  const registeredMarketplace = namedMarketplaces[0]
  const installedPlugin = ownedPlugins[0]
  if (!registeredMarketplace && installedPlugin) {
    fail(`The ${client} ${PLUGIN_NAME} plugin is installed without its marketplace. Remove or repair that registration with the native client before retrying.`, 'ORPHANED_PLUGIN')
  }
  if (installedPlugin && pluginMarketplace(client, installedPlugin) !== PLUGIN_NAME) {
    fail(`The ${client} ${PLUGIN_NAME} plugin is associated with a different marketplace. Resolve that registration with the native client before retrying.`, 'FOREIGN_PLUGIN')
  }
  if (installedPlugin?.enabled === false) {
    const action = client === 'codex'
      ? 'Enable it in the Codex plugin settings, then retry.'
      : `Run \`claude plugin enable ${PLUGIN_NAME}@${PLUGIN_NAME}\` for its current scope, then retry.`
    fail(`The ${client} ${PLUGIN_NAME} plugin is installed but disabled. ${action}`, 'PLUGIN_DISABLED')
  }

  if (!registeredMarketplace) {
    return {
      operations: clientCommands(client, installDirectory).map((item, index) => ({
        ...item,
        kind: index === 0 ? 'marketplace-add' : 'plugin-install',
      })),
      existingMarketplacePath: undefined,
      installedPlugin: undefined,
      marketplaceState: 'not-attempted',
      pluginState: 'not-attempted',
      pathMismatch: false,
    }
  }

  const existingPath = marketplacePath(client, registeredMarketplace)
  if (!existingPath) {
    fail(`The ${client} ${PLUGIN_NAME} marketplace has no local install path; resolve it with the native client before retrying.`, 'FOREIGN_MARKETPLACE')
  }
  if (existingPath === resolve(installDirectory)) {
    if (installedPlugin && installedPlugin.version !== packageVersion) {
      fail(`The registered ${client} plugin version is ${installedPlugin.version ?? 'unknown'}, but its local marketplace already points to this package version. The native client state is inconsistent; inspect its plugin list before retrying.`, 'PLUGIN_VERSION_MISMATCH')
    }
    return {
      operations: installedPlugin ? [] : [{
        kind: 'plugin-install',
        command: client === 'codex' ? 'codex' : 'claude',
        args: client === 'codex'
          ? ['plugin', 'add', `${PLUGIN_NAME}@${PLUGIN_NAME}`]
          : ['plugin', 'install', `${PLUGIN_NAME}@${PLUGIN_NAME}`],
      }],
      existingMarketplacePath: existingPath,
      installedPlugin,
      marketplaceState: 'already-registered',
      pluginState: installedPlugin ? 'already-installed' : 'not-attempted',
      pathMismatch: false,
    }
  }
  return {
    operations: [],
    existingMarketplacePath: existingPath,
    installedPlugin,
    marketplaceState: 'registered-old',
    pluginState: installedPlugin ? 'installed' : 'not-attempted',
    pathMismatch: true,
  }
}

async function assertOwnedPreviousInstall(path, dataDirectory) {
  const pluginsDirectory = resolve(dataDirectory, 'plugins')
  const candidate = resolve(path)
  const rel = relative(pluginsDirectory, candidate)
  const parts = rel.split(sep).filter(Boolean)
  if (!rel || rel.startsWith(`..${sep}`) || rel === '..' || isAbsolute(rel) || parts.length !== 1) {
    fail('An existing ESP-VISION marketplace points outside this installer data directory. Remove or repair it with the native client before upgrading.', 'FOREIGN_MARKETPLACE')
  }
  let marker
  try {
    await assertSafeDirectoryTree(candidate)
    const markerPath = join(candidate, INSTALL_MARKER)
    const info = await lstat(markerPath)
    if (!info.isFile() || info.isSymbolicLink()) throw new Error('invalid marker')
    marker = JSON.parse(await readFile(markerPath, 'utf8'))
    if (marker?.name !== PACKAGE_NAME || marker?.version !== parts[0] || !/^[a-f0-9]{64}$/.test(marker?.bundleHash ?? '')) {
      throw new Error('unrecognized marker')
    }
    const payload = await validatePayload(candidate, marker.version)
    if (payload.bundleHash !== marker.bundleHash) throw new Error('bundle mismatch')
  } catch {
    fail('An existing ESP-VISION marketplace is not a valid installer-owned version under this data directory. Resolve it with the native client before upgrading.', 'FOREIGN_MARKETPLACE')
  }
  return marker
}

async function assertSafeDirectoryTree(directory, { create = false } = {}) {
  const absolute = resolve(directory)
  const root = parse(absolute).root
  const components = relative(root, absolute).split(sep).filter(Boolean)
  let current = root
  for (const component of components) {
    current = join(current, component)
    let info
    try {
      info = await lstat(current)
    } catch (error) {
      if (error.code !== 'ENOENT' || !create) throw error
      try {
        await mkdir(current)
      } catch (mkdirError) {
        if (mkdirError.code !== 'EEXIST') throw mkdirError
      }
      info = await lstat(current)
    }
    if (info.isSymbolicLink()) fail(`Install directory path contains a symlink: ${current}`, 'UNSAFE_DIRECTORY')
    if (!info.isDirectory()) fail(`Install directory path is not a directory: ${current}`, 'UNSAFE_DIRECTORY')
  }
  return absolute
}

async function installedTargetState(target, expected) {
  let info
  try {
    info = await lstat(target)
  } catch (error) {
    if (error.code === 'ENOENT') return { exists: false }
    throw error
  }
  if (info.isSymbolicLink()) fail(`Refusing to replace a symlink at the plugin install path: ${target}`, 'UNSAFE_DIRECTORY')
  if (!info.isDirectory()) fail(`Refusing to replace a foreign file at the plugin install path: ${target}`, 'FOREIGN_INSTALL')

  const markerPath = join(target, INSTALL_MARKER)
  let markerInfo
  try {
    markerInfo = await lstat(markerPath)
  } catch (error) {
    if (error.code === 'ENOENT') fail(`Refusing to replace an unrecognized directory at the plugin install path: ${target}`, 'FOREIGN_INSTALL')
    throw error
  }
  if (!markerInfo.isFile() || markerInfo.isSymbolicLink()) {
    fail(`Refusing to use an invalid installer marker at the plugin install path: ${target}`, 'FOREIGN_INSTALL')
  }
  let marker
  try {
    marker = JSON.parse(await readFile(markerPath, 'utf8'))
  } catch (error) {
    fail(`Refusing to use an invalid installer marker at the plugin install path: ${target}`, 'FOREIGN_INSTALL', error)
  }
  if (marker?.name !== PACKAGE_NAME || marker?.version !== expected.version) {
    fail(`Refusing to replace a foreign installation at the plugin install path: ${target}`, 'FOREIGN_INSTALL')
  }
  const installed = await validatePayload(target, expected.version)
  if (marker.bundleHash !== expected.bundleHash || installed.bundleHash !== expected.bundleHash) {
    fail(`The existing ESP-VISION ${expected.version} install differs from this package; it was left unchanged.`, 'VERSION_COLLISION')
  }
  return { exists: true, owned: true, marker }
}

async function copyOne(source, destination, mode, copyFileImpl) {
  await mkdir(dirname(destination), { recursive: true })
  await copyFileImpl(source, destination, constants.COPYFILE_EXCL)
  await chmod(destination, mode)
}

async function stageInstall({ packageInfo, installDirectory, copyFileImpl }) {
  const parent = dirname(installDirectory)
  const stagingDirectory = join(parent, `.${packageInfo.version}.staging-${process.pid}-${randomUUID()}`)
  await mkdir(stagingDirectory)
  try {
    for (const file of packageInfo.files) {
      await copyOne(
        resolve(packageInfo.packageRoot, file.path),
        resolve(stagingDirectory, file.path),
        file.mode,
        copyFileImpl,
      )
    }
    await writeFile(resolve(stagingDirectory, 'package.json'), await readFile(resolve(packageInfo.packageRoot, 'package.json')), { flag: 'wx' })
    await writeFile(resolve(stagingDirectory, INSTALL_MARKER), `${JSON.stringify({
      name: PACKAGE_NAME,
      version: packageInfo.version,
      bundleHash: packageInfo.bundleHash,
    }, null, 2)}\n`, { flag: 'wx', mode: 0o600 })

    const staged = await validatePayload(stagingDirectory, packageInfo.version)
    if (staged.bundleHash !== packageInfo.bundleHash) fail('The staged plugin bundle does not match the package.', 'COPY_FAILED')
    try {
      await rename(stagingDirectory, installDirectory)
      return { copied: true }
    } catch (error) {
      if (!['EEXIST', 'ENOTEMPTY'].includes(error.code)) throw error
      const current = await installedTargetState(installDirectory, packageInfo)
      if (!current.exists) throw error
      await rm(stagingDirectory, { recursive: true, force: true })
      return { copied: false }
    }
  } catch (error) {
    await rm(stagingDirectory, { recursive: true, force: true }).catch(() => {})
    throw error
  }
}

async function stageWorkbuddyExport({ packageInfo, workbuddyDirectory, artifacts, artifactHash, writeFileImpl }) {
  const parent = dirname(workbuddyDirectory)
  const stagingDirectory = join(parent, `.${packageInfo.version}.staging-${process.pid}-${randomUUID()}`)
  await mkdir(stagingDirectory)
  try {
    for (const [name, contents] of artifacts) {
      await writeFileImpl(join(stagingDirectory, name), contents, { flag: 'wx', mode: 0o644 })
    }
    await writeFileImpl(join(stagingDirectory, WORKBUDDY_EXPORT_MARKER), `${JSON.stringify({
      name: PACKAGE_NAME,
      version: packageInfo.version,
      artifactHash,
    }, null, 2)}\n`, { flag: 'wx', mode: 0o600 })
    try {
      await rename(stagingDirectory, workbuddyDirectory)
      return { exported: true }
    } catch (error) {
      if (!['EEXIST', 'ENOTEMPTY'].includes(error.code)) throw error
      const current = await inspectWorkbuddyExport(workbuddyDirectory, packageInfo.version, artifactHash)
      if (!current.exists) throw error
      await rm(stagingDirectory, { recursive: true, force: true })
      return { exported: false }
    }
  } catch (error) {
    await rm(stagingDirectory, { recursive: true, force: true }).catch(() => {})
    throw error
  }
}

async function prepareWorkbuddy({
  dataDirectory,
  dryRun,
  packageRoot,
  copyFileImpl,
  writeFileImpl,
  env,
  platform,
  home,
}) {
  const packageInfo = await validatePackage(packageRoot)
  const selectedDataDirectory = resolve(dataDirectory ?? defaultDataDirectory({ env, platform, home }))
  const installDirectory = resolve(selectedDataDirectory, 'plugins', packageInfo.version)
  const workbuddyDirectory = resolve(selectedDataDirectory, WORKBUDDY_EXPORT_DIRECTORY, packageInfo.version)
  const artifacts = await makeWorkbuddyArtifacts(packageInfo, installDirectory)
  let targetState
  try {
    targetState = await installedTargetState(installDirectory, packageInfo)
  } catch (error) {
    if (error instanceof InstallerError) throw error
    fail('Unable to inspect the existing plugin install path.', 'INSTALL_PATH_ERROR', error)
  }

  try {
    await assertSafeDirectoryTree(dirname(workbuddyDirectory))
  } catch (error) {
    if (error.code !== 'ENOENT') throw error
  }
  const exportState = await inspectWorkbuddyExport(workbuddyDirectory, packageInfo.version, artifacts.artifactHash)
  if (dryRun) {
    return {
      client: 'workbuddy',
      version: packageInfo.version,
      installDirectory,
      workbuddyDirectory,
      skillZip: join(workbuddyDirectory, 'esp-vision-development.zip'),
      mcpConfig: join(workbuddyDirectory, 'mcp.json'),
      installationState: 'manual-install-required',
      dryRun: true,
      copied: !targetState.exists,
      exported: !exportState.exists,
      commands: [],
    }
  }

  let copied = false
  let exported = false
  let phase = 'plugin-copy'
  try {
    await assertSafeDirectoryTree(dirname(installDirectory), { create: true })
    targetState = await installedTargetState(installDirectory, packageInfo)
    if (!targetState.exists) {
      const stagedInstall = await stageInstall({ packageInfo, installDirectory, copyFileImpl })
      copied = stagedInstall.copied
    }
    phase = 'workbuddy-export'
    await assertSafeDirectoryTree(dirname(workbuddyDirectory), { create: true })
    if (!exportState.exists) {
      const stagedExport = await stageWorkbuddyExport({
        packageInfo,
        workbuddyDirectory,
        artifacts: artifacts.artifacts,
        artifactHash: artifacts.artifactHash,
        writeFileImpl,
      })
      exported = stagedExport.exported
    }
  } catch (error) {
    if (error instanceof InstallerError) throw error
    const filesState = copied ? `were copied to ${installDirectory}`
      : targetState.exists ? `were already installed at ${installDirectory}`
        : `were not installed at ${installDirectory}`
    if (phase === 'plugin-copy') {
      fail(
        `The verified ESP-VISION files ${filesState}. No WorkBuddy configuration was edited; rerun the installer after resolving the local file-system error.`,
        'COPY_FAILED',
        error,
        { files: copied ? 'copied' : targetState.exists ? 'already-installed' : 'not-installed', workbuddyExport: 'not-attempted' },
      )
    }
    fail(
      `The verified ESP-VISION files ${filesState}. The WorkBuddy export could not be completed. No WorkBuddy configuration was edited. Rerun the installer to prepare the Skill ZIP and MCP JSON.`,
      'WORKBUDDY_EXPORT_FAILED',
      error,
      { files: copied ? 'copied' : targetState.exists ? 'already-installed' : 'not-installed', workbuddyExport: 'incomplete' },
    )
  }
  return {
    client: 'workbuddy',
    version: packageInfo.version,
    installDirectory,
    workbuddyDirectory,
    skillZip: join(workbuddyDirectory, 'esp-vision-development.zip'),
    mcpConfig: join(workbuddyDirectory, 'mcp.json'),
    installationState: 'manual-install-required',
    dryRun: false,
    copied,
    exported,
    commands: [],
  }
}

async function runNativeCommand(command, args) {
  return execFile(command, args, {
    shell: false,
    windowsHide: true,
    encoding: 'utf8',
    maxBuffer: 1024 * 1024,
  })
}

function commandFailureSummary(error) {
  if (error?.code === 'ENOENT') return 'the client executable was not found on PATH'
  const code = Number.isInteger(error?.status) ? error.status
    : Number.isInteger(error?.exitCode) ? error.exitCode
      : Number.isInteger(error?.code) ? error.code : undefined
  if (code !== undefined) return `the client command exited with status ${code}`
  return 'the client command could not be started'
}

/** Prepare a verified plugin payload and either register a native client or export WorkBuddy artifacts. */
export async function installPlugin({
  client,
  dataDirectory,
  dryRun = false,
  packageRoot = PACKAGE_ROOT,
  commandRunner = runNativeCommand,
  copyFileImpl = copyFile,
  writeFileImpl = writeFile,
  env = process.env,
  platform = process.platform,
  home = homedir(),
} = {}) {
  if (client === 'workbuddy') {
    return prepareWorkbuddy({ dataDirectory, dryRun, packageRoot, copyFileImpl, writeFileImpl, env, platform, home })
  }
  if (client !== 'codex' && client !== 'claude') fail('Choose `codex`, `claude`, or `workbuddy` as the install target.', 'INVALID_ARGUMENTS')
  const packageInfo = await validatePackage(packageRoot)
  const selectedDataDirectory = resolve(dataDirectory ?? defaultDataDirectory({ platform, env, home }))
  const installDirectory = resolve(selectedDataDirectory, 'plugins', packageInfo.version)
  let plan
  if (dryRun) {
    plan = {
      operations: clientCommands(client, installDirectory).map((item, index) => ({
        ...item,
        kind: index === 0 ? 'marketplace-add' : 'plugin-install',
      })),
      marketplaceState: 'not-attempted',
      pluginState: 'not-attempted',
      pathMismatch: false,
    }
  } else {
    const nativeState = await inspectClientState(client, commandRunner)
    plan = operationPlan(client, installDirectory, packageInfo.version, nativeState)
    if (plan.pathMismatch) {
      const marker = await assertOwnedPreviousInstall(plan.existingMarketplacePath, selectedDataDirectory)
      if (marker.version !== packageInfo.version) {
        if (plan.installedPlugin && plan.installedPlugin.version !== marker.version) {
          fail(`The ${client} plugin list reports version ${plan.installedPlugin.version ?? 'unknown'}, but the registered installer-owned marketplace contains version ${marker.version}. No client changes were made. Resolve the native version mismatch before upgrading.`, 'PLUGIN_VERSION_MISMATCH')
        }
        plan = {
          ...plan,
          operations: upgradeCommands(client, installDirectory, plan.installedPlugin),
          marketplaceState: 'registered-previous',
          pluginState: plan.installedPlugin ? 'installed-previous' : 'not-installed',
          previousVersion: marker.version,
          previousDirectory: plan.existingMarketplacePath,
        }
      } else {
        fail(`The current ${client} ${PLUGIN_NAME} marketplace points to a different installer-owned path for version ${packageInfo.version}. Resolve it with the native client before retrying.`, 'INSTALL_PATH_MISMATCH')
      }
    }
  }
  const commands = plan.operations.map(({ command, args }) => ({ command, args }))
  let targetState
  try {
    targetState = await installedTargetState(installDirectory, packageInfo)
  } catch (error) {
    if (error instanceof InstallerError) throw error
    fail('Unable to inspect the existing plugin install path.', 'INSTALL_PATH_ERROR', error)
  }

  if (dryRun) {
    return {
      client,
      version: packageInfo.version,
      installDirectory,
      dryRun: true,
      copied: !targetState.exists,
      commands,
    }
  }

  let copied = false
  try {
    const parent = await assertSafeDirectoryTree(dirname(installDirectory), { create: true })
    if (parent !== dirname(installDirectory)) fail('The resolved plugin install path changed unexpectedly.', 'UNSAFE_DIRECTORY')
    targetState = await installedTargetState(installDirectory, packageInfo)
    if (!targetState.exists) {
      const staged = await stageInstall({ packageInfo, installDirectory, copyFileImpl })
      copied = staged.copied
    }
  } catch (error) {
    if (error instanceof InstallerError) throw error
    fail(
      `Could not safely copy the ESP-VISION ${packageInfo.version} plugin. No client commands were run; existing versioned installs were left unchanged.`,
      'COPY_FAILED',
      error,
      { files: 'not-installed', marketplace: 'not-attempted', plugin: 'not-attempted' },
    )
  }

  let marketplaceState = plan.marketplaceState
  let pluginState = plan.pluginState
  for (const command of plan.operations) {
    const operation = command.kind === 'marketplace-add' || command.kind === 'marketplace-remove'
      ? 'marketplace registration'
      : command.kind === 'plugin-remove' ? 'plugin removal' : 'plugin installation'
    try {
      await commandRunner(command.command, [...command.args])
      if (command.kind === 'marketplace-add') marketplaceState = 'completed'
      else if (command.kind === 'marketplace-remove') marketplaceState = 'removed'
      else if (command.kind === 'plugin-remove') pluginState = 'removed'
      else pluginState = 'completed'
    } catch (error) {
      let observed = { marketplace: 'unknown', plugin: 'unknown' }
      try {
        const state = await inspectClientState(client, commandRunner)
        observed = summarizeClientState(client, {
          ...state,
          expectedVersion: packageInfo.version,
        }, installDirectory, plan.previousDirectory)
      } catch { /* The native state is reported as unknown and can be inspected on retry. */ }
      const filesDescription = copied
        ? `The plugin copy is ready at ${installDirectory}.`
        : `The existing plugin copy is ready at ${installDirectory}.`
      fail(
        `${filesDescription} The ${client} ${operation} command failed because ${commandFailureSummary(error)}. Verified native state: marketplace ${observed.marketplace}; plugin ${observed.plugin}. The previous version files${plan.previousDirectory ? ` at ${plan.previousDirectory}` : ''} were left in place. Resolve the client error, inspect its plugin list if needed, then rerun this installer. No host configuration files were edited directly.`,
        'REGISTRATION_FAILED',
        error,
        {
          files: copied ? 'copied' : 'already-installed',
          previousVersion: plan.previousVersion,
          previousFiles: plan.previousDirectory ? 'preserved' : undefined,
          failedOperation: command.kind,
          ...observed,
        },
      )
    }
  }
  let finalState
  try {
    finalState = await inspectClientState(client, commandRunner)
  } catch (error) {
    fail(
      `The ${client} commands completed, but the installed state could not be verified. The plugin files are at ${installDirectory}; inspect the native marketplace and plugin lists, then rerun this installer.`,
      'REGISTRATION_UNVERIFIED', error,
      { files: copied ? 'copied' : 'already-installed', marketplace: 'unknown', plugin: 'unknown' },
    )
  }
  verifyInstalledState(client, finalState, installDirectory, packageInfo.version)
  return {
    client,
      version: packageInfo.version,
      installDirectory,
      dryRun: false,
      copied,
      marketplaceState,
      pluginState,
    commands,
  }
}

export const HELP_TEXT = `Usage: npx ${PACKAGE_NAME} install <codex|claude|workbuddy> [--dry-run] [--data-dir DIR]

Install the ESP-VISION plugin into a stable per-user data directory. Codex and Claude
Code are registered with their native CLIs. WorkBuddy receives a local Skill ZIP and
Knowledge MCP JSON for import through its documented UI; no WorkBuddy files are edited.

Options:
  --dry-run       Validate the packaged bundle and print the plan without writing or running commands.
  --data-dir DIR  Use DIR as the ESP-VISION data root (install goes under DIR/plugins/<version>).
  -h, --help      Show this help.
`

export async function main(argv = process.argv.slice(2), { stdout = process.stdout, stderr = process.stderr } = {}) {
  try {
    const parsed = parseInstallArgs(argv)
    if (parsed.help) {
      stdout.write(HELP_TEXT)
      return 0
    }
    const result = await installPlugin(parsed)
    const prefix = result.dryRun ? 'Dry run:' : result.client === 'workbuddy' ? 'Prepared:' : 'Installed:'
    stdout.write(`${prefix} ESP-VISION ${result.version} for ${result.client} at ${result.installDirectory}\n`)
    for (const command of result.commands) stdout.write(`  ${command.command} ${command.args.map(value => JSON.stringify(value)).join(' ')}\n`)
    if (result.client === 'workbuddy') {
      stdout.write(`  Skill ZIP: ${result.skillZip}\n  Knowledge MCP JSON: ${result.mcpConfig}\n  WorkBuddy setup guide: ${join(result.workbuddyDirectory, 'INSTALL.md')}\n`)
      if (!result.dryRun) stdout.write('manual-install-required: import the Skill ZIP and configure the Knowledge MCP in WorkBuddy, then start a new conversation. The WorkBuddy app has not been modified.\n')
    } else if (!result.dryRun) stdout.write('The plugin runtime is stored outside the npx cache. Start a new client session to load it.\n')
    return 0
  } catch (error) {
    const message = error instanceof InstallerError ? error.message : 'The ESP-VISION plugin installer failed.'
    stderr.write(`${message}\n`)
    return 1
  }
}

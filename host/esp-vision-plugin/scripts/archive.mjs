// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { mkdir, mkdtemp, readFile, writeFile } from 'node:fs/promises'
import { dirname, resolve, relative, isAbsolute, basename } from 'node:path'
import { fileURLToPath } from 'node:url'
import { promisify } from 'node:util'
import { execFile as execFileCallback } from 'node:child_process'

const execFile = promisify(execFileCallback)
const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const repoRoot = resolve(packageRoot, '../..')
const pluginRoot = resolve(repoRoot, 'host/esp-vision-plugin')

function readOption(name) {
  const index = process.argv.indexOf(name)
  return index < 0 ? undefined : process.argv[index + 1]
}

const outputValue = readOption('--output-dir') ?? process.env.ESP_VISION_PLUGIN_ARTIFACT_DIR
if (!outputValue) throw new Error('Set ESP_VISION_PLUGIN_ARTIFACT_DIR or pass --output-dir <directory>.')
if (process.argv.includes('--output-dir') && process.argv.length !== process.argv.indexOf('--output-dir') + 2) {
  throw new Error('Usage: node scripts/archive.mjs --output-dir <directory>')
}
const outputDir = resolve(outputValue)
const relativeOutput = relative(pluginRoot, outputDir)
assert.ok(relativeOutput.startsWith('..') || isAbsolute(relativeOutput), 'archive output must be outside the plugin source directory')
const manifest = JSON.parse(await readFile(resolve(pluginRoot, 'plugin.json'), 'utf8'))
assert.match(manifest.version, /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/)
await mkdir(outputDir, { recursive: true })

const archiveName = `esp-vision-plugin-${manifest.version}.tar.gz`
const archivePath = resolve(outputDir, archiveName)
await execFile('tar', ['-czf', archivePath, '-C', dirname(pluginRoot), basename(pluginRoot)], {
  cwd: repoRoot,
  timeout: 60000,
  maxBuffer: 2 * 1024 * 1024,
})
const archiveBytes = await readFile(archivePath)
const digest = createHash('sha256').update(archiveBytes).digest('hex')
const checksumPath = `${archivePath}.sha256`
await writeFile(checksumPath, `${digest}  ${archiveName}\n`)

const verifyRoot = await mkdtemp(resolve(outputDir, `.verify-${manifest.version}-`))
await execFile('tar', ['-xzf', archivePath, '-C', verifyRoot], {
  cwd: repoRoot,
  timeout: 60000,
  maxBuffer: 2 * 1024 * 1024,
})
const extractedPlugin = resolve(verifyRoot, basename(pluginRoot))
const { stdout: smokeOutput } = await execFile(process.execPath, [
  resolve(packageRoot, 'scripts/smoke-plugin.mjs'),
  '--plugin-dir', extractedPlugin,
], {
  cwd: repoRoot,
  env: process.env,
  timeout: 60000,
  maxBuffer: 2 * 1024 * 1024,
  windowsHide: true,
})

process.stdout.write(`${smokeOutput.trim()}\nArchive: ${archivePath}\nSHA-256: ${digest}\nChecksum: ${checksumPath}\nExtracted verification copy: ${extractedPlugin}\n`)

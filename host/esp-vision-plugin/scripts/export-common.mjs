// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { lstat, mkdir, readFile, realpath, writeFile } from 'node:fs/promises'
import { isAbsolute, relative, resolve, sep } from 'node:path'
import { createZip, validatePackage } from '../src/installer.mjs'

export const skillName = 'esp-vision-development'
export const json = value => `${JSON.stringify(value, null, 2)}\n`
export const digest = bytes => createHash('sha256').update(bytes).digest('hex')
const inside = (parent, path) => {
  const rel = relative(parent, path)
  return rel === '' || (!isAbsolute(rel) && rel !== '..' && !rel.startsWith(`..${sep}`))
}
export async function readTemplate(directory, name, binary = false) {
  const path = resolve(directory, name)
  const info = await lstat(path)
  assert.ok(info.isFile() && !info.isSymbolicLink(), `Invalid template: ${name}`)
  return readFile(path, binary ? undefined : 'utf8')
}
export const isSourceCommit = value => typeof value === 'string' && value.length === 40 && /^[a-f0-9]{40}$/.test(value)
export function sourceCommit(source, env = process.env, label = 'Plugin') {
  const inCI = env.CI_COMMIT_SHA !== undefined || env.CI
  const commit = inCI ? env.CI_COMMIT_SHA ?? '' : execFileSync('git', ['-C', source, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim()
  assert.ok(isSourceCommit(commit), `${label}${inCI ? ' CI_COMMIT_SHA' : ' source commit'} must be a complete lowercase 40-character Git SHA`)
  return commit
}
export function validatePublisher(author) {
  assert.ok(typeof author?.name === 'string' && author.name.trim(), 'Publisher name must be nonblank')
  assert.match(author.email, /^[^\s@]+@[^\s@]+\.[^\s@]+$/, 'Publisher email must be valid')
}
export function renderListing(text, manifest, sourceVersion = manifest.version) {
  const values = { VERSION: manifest.version, SOURCE_VERSION: sourceVersion,
    DISPLAY_NAME: typeof manifest.displayName === 'string' ? manifest.displayName : manifest.displayName?.zh,
    AUTHOR_NAME: manifest.author.name, AUTHOR_EMAIL: manifest.author.email }
  return text.replace(/{{(VERSION|SOURCE_VERSION|DISPLAY_NAME|AUTHOR_NAME|AUTHOR_EMAIL)}}/g, (_, key) => values[key])
}
export function sharedMetadata(original, overrides = {}) {
  for (const key of ['name', 'author', 'homepage', 'repository', 'license', 'keywords']) {
    assert.ok(!Object.hasOwn(overrides, key), `Platform overrides cannot redefine shared identity: ${key}`)
  }
  validatePublisher(original.author)
  const { name, version, description, homepage, repository, license, keywords, author } = original
  return { name, version, description, homepage, repository, license, keywords,
    author: { name: author.name, email: author.email }, ...overrides }
}
export function deviceReference(reference, introduction) {
  const sections = reference.split('## Hardware API and capture context')
  assert.equal(sections.length, 2, 'Common hardware acceptance reference changed; review adaptation')
  return introduction + '\n\n## Hardware API and capture context' + sections[1]
    .replace('CLI=/absolute/installed/plugin/cli/device-cli.mjs', ': "${CLI:?Set CLI to the verified installed bundle path}"')
    .replace('PORT=/dev/selected-device', ': "${PORT:?Set PORT to the explicitly selected serial device}"')
}

/** Shared source validation, runtime/license closure, provenance and archive writer. */
export async function createExport({ sourceRoot, packageRoot, outputDirectory, rootName }) {
  assert.ok(outputDirectory, 'Pass --output-dir outside the plugin source')
  const source = await realpath(resolve(sourceRoot))
  const destination = resolve(outputDirectory)
  async function resolveDestination(path) {
    try { return await realpath(path) } catch (error) {
      if (error.code !== 'ENOENT') throw error
      const parent = resolve(path, '..')
      return resolve(await resolveDestination(parent), relative(parent, path))
    }
  }
  const output = await resolveDestination(destination)
  assert.ok(!inside(source, output) && !inside(await realpath(packageRoot), output), 'Output must be outside the plugin source (outside both plugin source directories)')
  const info = await validatePackage(source)
  const original = JSON.parse(await readFile(resolve(source, 'plugin.json'), 'utf8'))
  const knowledge = JSON.parse(await readFile(resolve(source, 'mcp.json'), 'utf8')).mcpServers
  assert.deepEqual(Object.keys(knowledge), ['esp-vision-knowledge'])
  const knowledgeUrl = knowledge['esp-vision-knowledge'].url
  const files = new Map()
  function add(path, contents, mode = 0o644) {
    assert.ok(!isAbsolute(path) && !path.includes('\\') && !path.split('/').some(part => !part || part === '..' || part === '.'), `Unsafe plugin path: ${path}`)
    assert.ok(!files.has(path), `Duplicate plugin path: ${path}`)
    files.set(path, { name: `${rootName}/${path}`, contents: Buffer.from(contents), mode })
  }
  for (const file of info.files.filter(file => /^(cli|assets)\//.test(file.path) || file.path.startsWith(`skills/${skillName}/scripts/`))) {
    add(file.path, await readFile(resolve(source, file.path)), file.mode & 0o111 ? 0o755 : 0o644)
  }
  for (const path of ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'DEVICE_CORE_NOTICE.md']) add(path, await readFile(resolve(source, path)))
  function provenance(target, version, commit) {
    add('BUILD-PROVENANCE.json', json({ schemaVersion: 1, target, version, sourceVersion: info.version, sourceCommit: commit,
      files: [...files].map(([path, entry]) => ({ path, sha256: digest(entry.contents) })) }))
  }
  async function finish(name, receiptFactory) {
    await mkdir(output, { recursive: true })
    const actual = await realpath(output)
    assert.ok(!inside(source, actual) && !inside(await realpath(packageRoot), actual), 'Output must be outside the plugin source (outside both plugin source directories)')
    const entries = [...files.values()].sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0)
    const archive = createZip(entries)
    const sha256 = digest(archive)
    const inventory = entries.map(entry => ({ path: entry.name.slice(rootName.length + 1), bytes: entry.contents.length, sha256: digest(entry.contents) }))
    const receipt = receiptFactory?.({ sha256, inventory })
    const artifacts = new Map([[name, archive], [`${name}.sha256`, `${sha256}  ${name}\n`]])
    if (receipt) artifacts.set(`${name}.receipt.json`, json(receipt))
    for (const filename of artifacts.keys()) {
      await assert.rejects(lstat(resolve(output, filename)), error => error.code === 'ENOENT', `Artifact already exists; refusing to overwrite: ${filename}`)
    }
    for (const [filename, bytes] of artifacts) await writeFile(resolve(output, filename), bytes, { flag: 'wx' })
    return { ...receipt, archivePath: resolve(output, name), sha256 }
  }
  return { source, info, original, knowledgeUrl, files, add, provenance, finish }
}

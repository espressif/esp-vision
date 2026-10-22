// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { mkdtemp, mkdir, readFile, rm, stat, writeFile } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { basename, dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../../..')
const registry = 'https://registry.npmjs.org/'
const configPackagePaths = new Map([
  ['host/device-cli', resolve(repoRoot, 'host/device-cli')],
  ['host/knowledge-mcp', resolve(repoRoot, 'host/knowledge-mcp')],
  ['host/esp-vision-plugin', resolve(repoRoot, 'host/esp-vision-plugin')],
])

function parseArgs(argv) {
  let dryRun = false
  let packagePath
  let suppliedTarball
  let tarballDirectory
  for (let index = 0; index < argv.length; index++) {
    if (argv[index] === '--dry-run') {
      assert.equal(dryRun, false, '--dry-run may only be specified once')
      dryRun = true
    } else if (argv[index] === '--tarball-dir') {
      assert.ok(argv[index + 1] && !tarballDirectory, 'Expected one --tarball-dir DIRECTORY')
      tarballDirectory = resolve(argv[++index])
    } else if (argv[index] === '--tarball') {
      assert.ok(argv[index + 1] && !suppliedTarball, 'Expected one --tarball PATH')
      suppliedTarball = resolve(argv[++index])
    } else if (argv[index] === '--package') {
      assert.ok(argv[index + 1], 'Expected --package <host/device-cli|host/knowledge-mcp|host/esp-vision-plugin>')
      assert.equal(packagePath, undefined, '--package may only be specified once')
      packagePath = argv[++index]
    } else {
      throw new Error('Usage: node scripts/release-npm.mjs [--dry-run] --package <host/device-cli|host/knowledge-mcp|host/esp-vision-plugin>')
    }
  }
  assert.ok(configPackagePaths.has(packagePath), 'Package must be exactly host/device-cli, host/knowledge-mcp or host/esp-vision-plugin')
  assert.ok(!(suppliedTarball && tarballDirectory), 'Choose --tarball or --tarball-dir')
  return { dryRun, suppliedTarball, tarballDirectory, packagePath, packageRoot: configPackagePaths.get(packagePath) }
}

const { dryRun, suppliedTarball: suppliedPath, tarballDirectory, packagePath, packageRoot } = parseArgs(process.argv.slice(2))
const sourceManifest = JSON.parse(await readFile(join(packageRoot, 'package.json'), 'utf8'))
assert.match(sourceManifest.name, /^@esp-vision\/[a-z0-9-]+$/)
const suppliedTarball = suppliedPath ?? (tarballDirectory ? join(tarballDirectory, `${sourceManifest.name.slice(1).replace('/', '-')}-${sourceManifest.version}.tgz`) : undefined)
assert.match(sourceManifest.version, /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/)
assert.notEqual(sourceManifest.private, true, 'The selected npm package must be publishable')
assert.equal(sourceManifest.license, 'Apache-2.0')
assert.equal(sourceManifest.publishConfig?.registry, registry, 'Package publishConfig must target npmjs')
assert.equal(sourceManifest.publishConfig?.access, 'public', 'Scoped packages must be published publicly')
assert.deepEqual(sourceManifest.dependencies ?? {}, {}, 'Standalone ESP-VISION packages must not have runtime dependencies')
for (const dependencyGroup of ['dependencies', 'devDependencies', 'optionalDependencies', 'peerDependencies']) {
  for (const [name, version] of Object.entries(sourceManifest[dependencyGroup] ?? {})) {
    assert.ok(!String(version).startsWith('file:'), `${dependencyGroup}.${name} must not refer to a local file`)
  }
}

if (!dryRun) {
  assert.equal(process.env.CI_PIPELINE_SOURCE, 'push', 'Publish only from the merged default-branch pipeline')
  if (packagePath === 'host/knowledge-mcp') {
    assert.match(process.env.CI_COMMIT_TAG ?? '', /^(?:v\d+\.\d+(?:\.\d+)?|\d{4}\.\d{1,2}\.\d{1,2})(?:$|[-.])/,
      'Knowledge npm publication requires a protected firmware release tag')
    assert.ok(suppliedTarball, 'Publish the exact Knowledge tarball used by the image')
  } else {
    assert.ok(process.env.CI_DEFAULT_BRANCH && process.env.CI_COMMIT_BRANCH === process.env.CI_DEFAULT_BRANCH,
      'Publish only after a manual merge to the default branch')
  }
  assert.equal(process.env.CI_COMMIT_REF_PROTECTED, 'true', 'Publishing requires a protected branch')
  assert.ok(process.env.NPM_PUBLISH_TOKEN, 'Protected NPM_PUBLISH_TOKEN is missing')
}

const tempBase = resolve(process.env.ESP_VISION_NPM_TEMP_DIR ?? process.env.TMPDIR ?? join(repoRoot, '.cache/npm-release'))
const artifacts = resolve(process.env.ESP_VISION_NPM_TARBALL_DIR ?? join(tempBase, 'tarballs'))
await mkdir(tempBase, { recursive: true })
await mkdir(artifacts, { recursive: true })
const temporary = await mkdtemp(join(tempBase, 'release-'))
const authConfig = join(temporary, 'npmrc')
try {
  let metadata
  let tarball = suppliedTarball
  if (!tarball) {
    if (packagePath === 'host/esp-vision-plugin') runNpm(['run', 'build'], packageRoot)
    else runNpm(['run', packagePath === 'host/knowledge-mcp' ? 'build:release' : 'build'], packageRoot)
    const packed = runNpm(['pack', '--json', '--ignore-scripts', '--pack-destination', artifacts], packageRoot)
    ;[metadata] = JSON.parse(packed.stdout)
    tarball = join(artifacts, metadata.filename)
  } else {
    const listing = spawnSync('tar', ['-tzf', tarball], { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 })
    assert.equal(listing.status, 0, 'Could not list supplied npm tarball')
    const types = spawnSync('tar', ['-tvzf', tarball], { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 })
    assert.equal(types.status, 0, 'Could not inspect supplied npm archive types')
    assert.ok(types.stdout.trim().split('\n').every(line => /^[-d]/.test(line)), 'Npm archives may contain only regular files and directories')
    const entries = listing.stdout.trim().split('\n')
    for (const path of entries) {
      assert.ok(path.startsWith('package/') && !path.split('/').includes('..'), 'Unsafe npm archive path')
    }
    metadata = { name: sourceManifest.name, version: sourceManifest.version, filename: basename(tarball),
      integrity: 'sha512-' + createHash('sha512').update(await readFile(tarball)).digest('base64'),
      files: entries.filter(path => !path.endsWith('/')).map(path => ({ path: path.slice('package/'.length) })) }
  }
  assert.equal(metadata.name, sourceManifest.name)
  assert.equal(metadata.version, sourceManifest.version)
  assert.match(metadata.integrity, /^sha512-/)
  const packedRoot = await verifyTarball(tarball, metadata, sourceManifest, temporary)

  if (packagePath === 'host/esp-vision-plugin') {
    await verifyPluginInstaller(packedRoot, temporary, sourceManifest.version)
  } else {
    runNode([join(packageRoot, 'scripts/check-package.mjs'), '--tarball', tarball], packageRoot)
  }

  if (dryRun) {
    // CI qualifies archives even when this package version is already public.
    // npm publish --dry-run rejects those versions before any registry write.
    const [checked] = JSON.parse(runNpm(['pack', tarball, '--dry-run', '--ignore-scripts', '--json'], packageRoot).stdout)
    assert.equal(checked.name, metadata.name)
    assert.equal(checked.version, metadata.version)
    assert.equal(checked.integrity, metadata.integrity, 'npm interpreted a different tarball')
    console.log(`Archive validation verified ${metadata.name}@${metadata.version}; integrity ${metadata.integrity}; tarball ${tarball}; no registry write`)
  } else {
    await writeFile(authConfig, `registry=${registry}\n//registry.npmjs.org/:_authToken=\${NPM_PUBLISH_TOKEN}\n`, { mode: 0o600 })
    const publisher = runNpm(['whoami', '--registry', registry], packageRoot, { authConfig }).stdout.trim()
    if (publisher !== 'esp-vision') throw new Error('Npm publisher account must be esp-vision; no package was published.')
    console.log('Npm publisher identity verified.')

    const pinnedCore = sourceManifest.devDependencies?.['@esp-vision/device-core']
    if (pinnedCore) {
      const lock = JSON.parse(await readFile(join(packageRoot, 'package-lock.json'), 'utf8'))
      const expected = lock.packages?.['node_modules/@esp-vision/device-core']
      assert.equal(expected?.version, pinnedCore, 'Core lock version must match the declared build dependency')
      assert.match(expected?.integrity ?? '', /^sha512-/, 'Core lock must bind the tested tarball bytes')
      const publishedCore = await readRegistryMetadata(new URL(`${encodeURIComponent('@esp-vision/device-core')}/${pinnedCore}`, registry))
      assert.ok(publishedCore, 'Publish the pinned Core version before publishing its consumers')
      assert.equal(publishedCore.dist?.integrity, expected.integrity, 'Published Core differs from the tested build artifact')
    }

    const versionUrl = new URL(`${encodeURIComponent(sourceManifest.name)}/${encodeURIComponent(sourceManifest.version)}`, registry)
    const before = await readRegistryMetadata(versionUrl)
    if (before) {
      assert.equal(before.dist?.integrity, metadata.integrity,
        'This version already exists with different content; bump the package version')
      console.log(`${metadata.name}@${metadata.version} is already published with identical content; skipped.`)
    } else {
      const published = runNpmResult(
        ['publish', tarball, '--ignore-scripts', '--access', 'public', '--registry', registry],
        packageRoot,
        { authConfig },
      )
      if (published.status !== 0) {
        // A network failure can leave npm unsure whether its single write was
        // accepted. Read metadata, but never retry the publish command here.
        try {
          await verifyPublishedIntegrity(versionUrl, metadata.integrity)
          console.log(`${metadata.name}@${metadata.version} was accepted; integrity verified after the publish client reported an error.`)
        } catch (verificationError) {
          throw new Error(`npm publish failed with exit ${published.status}; the write was not repeated. Registry check: ${safeText(verificationError.message)}`)
        }
      } else {
        console.log('npm publish accepted; verifying registry metadata.')
        await verifyPublishedIntegrity(versionUrl, metadata.integrity)
        console.log(`Published and verified ${metadata.name}@${metadata.version}.`)
      }
    }
  }
} finally {
  await rm(temporary, { recursive: true, force: true })
}

function runNpm(args, cwd, { authConfig: configPath } = {}) {
  const result = runNpmResult(args, cwd, { authConfig: configPath })
  if (result.status !== 0) {
    const detail = safeText(result.stderr || result.error?.message || '')
    throw new Error(`npm ${args[0]} failed with exit ${result.status ?? 'unknown'}${detail ? `: ${detail}` : ''}`)
  }
  return result
}

function runNpmResult(args, cwd, { authConfig: configPath } = {}) {
  const env = { ...process.env }
  if (configPath) env.NPM_CONFIG_USERCONFIG = configPath
  const result = spawnSync('npm', args, { cwd, env, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 })
  if (result.error) return { ...result, status: null }
  if (result.status !== 0) {
    const detail = safeText(result.stderr || result.error?.message || '')
    return { ...result, stderr: detail }
  }
  return result
}

function runNode(args, cwd) {
  const result = spawnSync(process.execPath, args, { cwd, env: process.env, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 })
  assert.equal(result.status, 0, `Node check failed: ${safeText(result.stderr || result.error?.message || 'unknown error')}`)
}

function safeText(value) {
  const token = process.env.NPM_PUBLISH_TOKEN
  return token ? String(value).replaceAll(token, '[redacted]') : String(value)
}

async function verifyTarball(tarball, metadata, manifest, temporaryRoot) {
  const allowed = manifest.files ?? []
  const files = metadata.files.map(({ path }) => path)
  assert.ok(files.includes('package.json'))
  const binPaths = Object.values(manifest.bin ?? {}).map(value => String(value).replace(/^\.\//, ''))
  assert.ok(binPaths.length > 0, 'Published executable package must declare a bin entry')
  for (const binPath of binPaths) assert.ok(files.includes(binPath), `npm tarball is missing bin path ${binPath}`)
  for (const path of files) {
    const listed = path === 'package.json' || /^README(?:\.[^/]*)?$/i.test(path) || /^LICEN[CS]E(?:\.[^/]*)?$/i.test(path)
      || allowed.some(entry => path === entry.replace(/\/$/, '') || path.startsWith(entry.endsWith('/') ? entry : `${entry}/`))
    assert.ok(listed, `npm tarball contains a file outside the package allowlist: ${path}`)
  }

  const extracted = join(temporaryRoot, 'unpacked')
  await mkdir(extracted, { recursive: true })
  const unpack = spawnSync('tar', ['-xzf', tarball, '-C', extracted], { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 })
  assert.equal(unpack.status, 0, `Could not extract npm tarball: ${unpack.stderr || unpack.error?.message || 'unknown error'}`)
  const packedRoot = join(extracted, 'package')
  const packedManifest = JSON.parse(await readFile(join(packedRoot, 'package.json'), 'utf8'))
  assert.equal(packedManifest.name, manifest.name)
  assert.equal(packedManifest.version, manifest.version)
  assert.deepEqual(packedManifest.dependencies ?? {}, {})
  for (const dependencyGroup of ['dependencies', 'devDependencies', 'optionalDependencies', 'peerDependencies']) {
    for (const [name, version] of Object.entries(packedManifest[dependencyGroup] ?? {})) {
      assert.ok(!String(version).startsWith('file:'), `Packed ${dependencyGroup}.${name} refers to a local path`)
    }
  }
  for (const binPath of binPaths) {
    const bin = await readFile(join(packedRoot, binPath), 'utf8')
    assert.ok(bin.startsWith('#!/usr/bin/env node'), `Packed bin ${binPath} must have a Node shebang`)
    const info = await stat(join(packedRoot, binPath))
    assert.ok(info.mode & 0o100, `Packed bin ${binPath} must be executable`)
  }
  console.log(`Tarball manifest and allowlist verified: ${manifest.name}@${manifest.version} (${files.length} files).`)
  return packedRoot
}

async function verifyPluginInstaller(packedRoot, temporaryRoot, version) {
  const dataRoot = join(temporaryRoot, 'installer-dry-run-data')
  for (const client of ['codex', 'claude']) {
    const env = { ...process.env }
    delete env.NODE_PATH
    delete env.NODE_OPTIONS
    const result = spawnSync(process.execPath, [join(packedRoot, 'bin/esp-vision-plugin.mjs'), 'install', client, '--dry-run', '--data-dir', dataRoot], {
      cwd: packedRoot,
      env,
      encoding: 'utf8',
      maxBuffer: 8 * 1024 * 1024,
    })
    assert.equal(result.status, 0, `Extracted plugin installer ${client} dry run failed: ${safeText(result.stderr || result.error?.message || 'unknown error')}`)
    assert.ok(result.stdout.startsWith(`Dry run: ESP-VISION ${version} for ${client}`))
  }
  console.log('Extracted npm installer dry runs passed for Codex and Claude.')
}

async function readRegistryMetadata(url) {
  const attempts = 10
  for (let attempt = 0; attempt < attempts; attempt++) {
    let response
    try {
      response = await fetch(url, {
        cache: 'no-store',
        headers: { 'Cache-Control': 'no-cache' },
        signal: AbortSignal.timeout(15000),
      })
    } catch (error) {
      if (!(error instanceof TypeError) && !['TimeoutError', 'AbortError'].includes(error.name)) throw error
      if (attempt + 1 === attempts) throw new Error(`npm registry metadata read failed after ${attempts} attempts (${error.name})`)
      await retryRead(attempt, `network ${error.name}`)
      continue
    }
    if (response.ok) return await response.json()
    if (response.status === 404) return null
    const retryable = response.status === 408 || response.status === 429 || response.status >= 500 && response.status <= 599
    await response.body?.cancel()
    assert.ok(retryable, `npm registry metadata read failed with HTTP ${response.status}`)
    if (attempt + 1 === attempts) throw new Error(`npm registry metadata read failed after ${attempts} attempts (HTTP ${response.status})`)
    await retryRead(attempt, `HTTP ${response.status}`)
  }
  throw new Error('npm registry metadata read failed')
}

async function verifyPublishedIntegrity(url, integrity) {
  const attempts = 10
  for (let attempt = 0; attempt < attempts; attempt++) {
    const metadata = await readRegistryMetadata(url)
    if (metadata) {
      assert.equal(metadata.dist?.integrity, integrity, 'Published tarball integrity mismatch')
      return
    }
    if (attempt + 1 === attempts) {
      throw new Error(`Published metadata was not visible after ${attempts} read attempts; do not repeat the publish command blindly`)
    }
    await retryRead(attempt, 'HTTP 404')
  }
}

async function retryRead(attempt, reason) {
  const delay = Math.min(30000, 1000 * 2 ** attempt)
  console.log(`Registry metadata read ${reason}; retrying read ${attempt + 2}/10 in ${delay}ms.`)
  await new Promise(resolvePromise => setTimeout(resolvePromise, delay))
}

#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Exercise the published plugin installer against real Codex and Claude CLIs
// while bubblewrap redirects their user config paths into an isolated output directory.
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import {
  chmod,
  copyFile,
  cp,
  lstat,
  mkdir,
  readFile,
  realpath,
  writeFile,
} from 'node:fs/promises'
import { homedir } from 'node:os'
import { dirname, isAbsolute, join, resolve } from 'node:path'

const PACKAGE_NAME = '@esp-vision/plugin'
const PLUGIN_NAME = 'esp-vision'
const FAILURE_EXIT = 73
const COMMAND_WATCHDOG_MS = 120_000
const args = parseArgs(process.argv.slice(2))
let activeRunRoot
let activeSummary

if (args.help) {
  console.log(`Usage: node tools/ci/mcp/native_plugin_acceptance.mjs --tarball NEW --previous-tarball OLD --codex ABS_REAL_BINARY --claude ABS_REAL_BINARY --output PROJECTDIR

Run real Codex and Claude plugin installs in Linux bubblewrap sandboxes. The
  script preserves client state, tarball installs, command logs, and results under
  PROJECTDIR. PROJECTDIR must be absolute. It never changes HOME or CODEX_HOME.

Required:
  --tarball PATH          Current @esp-vision/plugin npm tarball
Optional:
  --previous-tarball PATH Previous public plugin tarball; defaults to an explicitly synthetic upgrade fixture
  --codex PATH            Absolute path to the real Codex executable
  --claude PATH           Absolute path to the real Claude Code executable
  --output PROJECTDIR     Absolute output directory for isolated state and logs
`)
  process.exit(0)
}

function parseArgs(argv) {
  const values = {}
  for (let index = 0; index < argv.length; index += 1) {
    const option = argv[index]
    if (option === '-h' || option === '--help') return { help: true }
    if (!['--tarball', '--previous-tarball', '--codex', '--claude', '--output'].includes(option)) {
      throw new Error(`Unknown option: ${option}`)
    }
    if (values[option]) throw new Error(`Option may only be used once: ${option}`)
    const value = argv[index + 1]
    if (!value || value.startsWith('--')) throw new Error(`${option} requires a value`)
    values[option] = value
    index += 1
  }
  for (const option of ['--tarball', '--codex', '--claude', '--output']) {
    if (!values[option]) throw new Error(`Missing required option: ${option}`)
  }
  if (!isAbsolute(values['--output'])) throw new Error('--output must be an absolute path.')
  return {
    tarball: values['--tarball'],
    previousTarball: values['--previous-tarball'],
    codex: values['--codex'],
    claude: values['--claude'],
    output: values['--output'],
  }
}

function run(command, commandArgs, { cwd, env } = {}) {
  const result = spawnSync(command, commandArgs, {
    cwd,
    env,
    encoding: 'utf8',
    maxBuffer: 32 * 1024 * 1024,
    timeout: COMMAND_WATCHDOG_MS,
  })
  return {
    status: result.status ?? (result.error?.code === 'ETIMEDOUT' ? 124 : 1),
    rawStatus: result.status,
    stdout: result.stdout ?? '',
    stderr: result.stderr ?? '',
    signal: result.signal,
    error: result.error ? { code: result.error.code, message: result.error.message } : undefined,
  }
}

function assertCommandSucceeded(result, label) {
  if (result.error || result.status !== 0) {
    const details = result.stderr || result.error?.message || `exit status ${result.status}`
    throw new Error(`${label} failed: ${details}`)
  }
}

async function writeJson(path, value) {
  await writeFile(path, `${JSON.stringify(value, null, 2)}\n`)
}

async function assertExecutable(path, label) {
  if (!isAbsolute(path)) throw new Error(`${label} path must be absolute: ${path}`)
  const resolvedPath = await realpath(path)
  const info = await lstat(resolvedPath)
  if (!info.isFile() || (info.mode & 0o111) === 0) throw new Error(`${label} must resolve to an executable regular file.`)
  return resolvedPath
}

async function findExecutable(command) {
  for (const directory of (process.env.PATH ?? '').split(':').filter(Boolean)) {
    try {
      return await assertExecutable(join(directory, command), command)
    } catch (error) {
      if (!['ENOENT', 'ENOTDIR'].includes(error.code) && !/must resolve to an executable regular file/.test(error.message)) throw error
    }
  }
  return undefined
}

async function assertNativeMountTarget(path, kind) {
  const info = await lstat(path).catch(error => {
    if (error.code === 'ENOENT') throw new Error(`Native client config path does not exist: ${path}`)
    throw error
  })
  if (info.isSymbolicLink()) throw new Error(`Refusing to overlay a symbolic link at native client config path: ${path}`)
  if (kind === 'directory' ? !info.isDirectory() : !info.isFile()) {
    throw new Error(`Native client config path has the wrong type: ${path}`)
  }
}

async function packageVersion(tarball, label, npmPath, env, outputDirectory) {
  const installRoot = join(outputDirectory, 'npm-install')
  await mkdir(installRoot, { recursive: true })
  const install = run(process.execPath, [npmPath, 'install', '--prefix', installRoot, '--ignore-scripts', '--no-audit', '--no-fund', '--package-lock=false', tarball], {
    cwd: installRoot,
    env,
  })
  await writeFile(join(outputDirectory, 'npm-install.stdout.log'), install.stdout)
  await writeFile(join(outputDirectory, 'npm-install.stderr.log'), install.stderr)
  await writeJson(join(outputDirectory, 'npm-install.result.json'), install)
  assertCommandSucceeded(install, `npm install ${label}`)
  const packageRoot = join(installRoot, 'node_modules', '@esp-vision', 'plugin')
  const manifest = JSON.parse(await readFile(join(packageRoot, 'package.json'), 'utf8'))
  assert.equal(manifest.name, PACKAGE_NAME, `${label} package name`)
  return { packageRoot, version: manifest.version }
}

async function createProfile(runRoot, name, { codexConfig, claudeConfig, claudeJson, nodePath, outputRoot, bwrapPath }) {
  const profileRoot = join(runRoot, 'profiles', name)
  const isolatedCodex = join(profileRoot, 'user', '.codex')
  const isolatedClaude = join(profileRoot, 'user', '.claude')
  const isolatedClaudeJson = join(profileRoot, 'user', '.claude.json')
  const tmp = join(profileRoot, 'tmp')
  const cache = join(profileRoot, 'cache')
  const bin = join(profileRoot, 'bin')
  await Promise.all([
    mkdir(isolatedCodex, { recursive: true }),
    mkdir(isolatedClaude, { recursive: true }),
    mkdir(tmp, { recursive: true }),
    mkdir(cache, { recursive: true }),
    mkdir(bin, { recursive: true }),
  ])
  await writeFile(isolatedClaudeJson, '{}\n')
  const commandLog = join(profileRoot, 'native-commands.log')
  const failMarker = join(profileRoot, 'marketplace-add-failed-once')
  await writeFile(commandLog, '')
  const target = name.includes('codex') ? 'codex' : 'claude'
  const realClient = target === 'codex' ? join(runRoot, 'bin', 'codex') : claudeConfig.binary
  const wrapper = join(bin, target)
  const wrapperSource = `#!/bin/sh
set +e
printf '%s' "${target}" >> "$ESP_VISION_COMMAND_LOG"
for arg in "$@"; do printf '\\t%s' "$arg" >> "$ESP_VISION_COMMAND_LOG"; done
printf '\\n' >> "$ESP_VISION_COMMAND_LOG"
if [ "${'${'}ESP_VISION_FAIL_MARKETPLACE_ADD:-0}" = "1" ] && [ "${'${'}1-}" = "plugin" ] && [ "${'${'}2-}" = "marketplace" ] && [ "${'${'}3-}" = "add" ] && [ ! -e "$ESP_VISION_FAIL_MARKER" ]; then
  "$ESP_VISION_REAL_CLIENT" "$@"
  status=$?
  if [ "$status" -ne 0 ]; then exit "$status"; fi
  : > "$ESP_VISION_FAIL_MARKER"
  printf 'injected-after-success exit=%s\\n' "${'${'}ESP_VISION_FAILURE_EXIT}" >> "$ESP_VISION_COMMAND_LOG"
  exit "$ESP_VISION_FAILURE_EXIT"
fi
"$ESP_VISION_REAL_CLIENT" "$@"
status=$?
printf 'status=%s\\n' "$status" >> "$ESP_VISION_COMMAND_LOG"
exit "$status"
`
  await writeFile(wrapper, wrapperSource, { mode: 0o755 })
  await chmod(wrapper, 0o755)
  const pathValue = [bin, dirname(nodePath), '/usr/local/bin', '/usr/bin', '/bin'].join(':')
  const bwrapArgs = [
    '--ro-bind', '/', '/',
    '--bind', outputRoot, outputRoot,
    '--bind', isolatedCodex, codexConfig,
    '--bind', isolatedClaude, claudeConfig.path,
    '--bind', isolatedClaudeJson, claudeJson,
    '--dev', '/dev',
    '--proc', '/proc',
    '--chdir', profileRoot,
    '--setenv', 'PATH', pathValue,
    '--setenv', 'TMPDIR', tmp,
    '--setenv', 'TMP', tmp,
    '--setenv', 'TEMP', tmp,
    '--setenv', 'XDG_CACHE_HOME', cache,
    '--setenv', 'ESP_VISION_REAL_CLIENT', realClient,
    '--setenv', 'ESP_VISION_COMMAND_LOG', commandLog,
    '--setenv', 'ESP_VISION_FAIL_MARKER', failMarker,
    '--setenv', 'ESP_VISION_FAILURE_EXIT', String(FAILURE_EXIT),
    '--',
  ]
  // HOME and CODEX_HOME are inherited unchanged and never assigned here.
  return {
    profileRoot,
    dataRoot: join(profileRoot, 'data'),
    commandLog,
    failMarker,
    wrapper,
    bwrapArgs,
    bwrapPath,
    nativeClient: target,
    isolatedPaths: { codex: isolatedCodex, claude: isolatedClaude, claudeJson: isolatedClaudeJson },
  }
}

function installerCommand(nodePath, packageRoot, client, dataRoot) {
  return [nodePath, join(packageRoot, 'bin', 'esp-vision-plugin.mjs'), 'install', client, '--data-dir', dataRoot]
}

async function invokeInstaller(profile, { nodePath, packageRoot, client, dataRoot, label, failOnce = false }) {
  const command = installerCommand(nodePath, packageRoot, client, dataRoot)
  const extra = failOnce ? ['--setenv', 'ESP_VISION_FAIL_MARKETPLACE_ADD', '1'] : ['--setenv', 'ESP_VISION_FAIL_MARKETPLACE_ADD', '0']
  const full = [...profile.bwrapArgs.slice(0, -1), ...extra, '--', ...command]
  const result = run(profile.bwrapPath, full, { cwd: profile.profileRoot })
  await writeJson(join(profile.profileRoot, `${label}.json`), { command, ...result })
  return result
}

async function assertPayload(dataRoot, version, relativePath) {
  const target = join(dataRoot, 'plugins', version, relativePath)
  const info = await lstat(target).catch(error => {
    if (error.code === 'ENOENT') throw new Error(`Expected installed payload is missing: ${target}`)
    throw error
  })
  assert.ok(info.isFile() && !info.isSymbolicLink(), `Expected regular installed payload: ${target}`)
  return target
}

async function syntheticPreviousTarball(packageRoot, runRoot, npmPath, env) {
  // This fixture exercises future upgrades; it is not a historical npm release.
  const fixture = join(runRoot, 'synthetic-previous-package')
  await cp(packageRoot, fixture, { recursive: true })
  for (const entry of ['package.json', 'plugin.json', '.claude-plugin/plugin.json']) {
    const path = join(fixture, entry)
    const manifest = JSON.parse(await readFile(path, 'utf8'))
    manifest.version = '0.0.0'
    await writeJson(path, manifest)
  }
  const packed = run(process.execPath, [npmPath, 'pack', '--json', '--ignore-scripts', '--pack-destination', runRoot], { cwd: fixture, env })
  assertCommandSucceeded(packed, 'pack synthetic previous plugin fixture')
  const [artifact] = JSON.parse(packed.stdout)
  return join(runRoot, artifact.filename)
}

async function scenario({ runRoot, name, client, codexConfig, claudeConfig, claudeJson, nodePath, outputRoot, npmEnv, npmPath, newTarball, oldTarball, newPackage, oldPackage, summary, bwrapPath }) {
  const profile = await createProfile(runRoot, name, { codexConfig, claudeConfig, claudeJson, nodePath, outputRoot, bwrapPath })
  const scenarioResult = { client, profile: profile.profileRoot, steps: [] }
  const record = async (label, result, expectation) => {
    scenarioResult.steps.push({ label, status: result.status, expectation, stdout: result.stdout, stderr: result.stderr, error: result.error })
    await writeJson(join(profile.profileRoot, 'scenario.json'), scenarioResult)
  }

  if (name.startsWith('first-')) {
    const installed = await packageVersion(newTarball, 'current plugin', npmPath, npmEnv, join(profile.profileRoot, 'current-package'))
    assert.equal(installed.version, newPackage.version)
    const first = await invokeInstaller(profile, { nodePath, packageRoot: installed.packageRoot, client, dataRoot: profile.dataRoot, label: 'first-install' })
    assert.equal(first.status, 0, `${client} first install: ${first.stderr}`)
    await assertPayload(profile.dataRoot, newPackage.version, 'cli/device-cli.mjs')
    await record('first-install', first, 'success')

    const repeat = await invokeInstaller(profile, { nodePath, packageRoot: installed.packageRoot, client, dataRoot: profile.dataRoot, label: 'repeat-install' })
    assert.equal(repeat.status, 0, `${client} repeat install: ${repeat.stderr}`)
    assert.ok(!repeat.stdout.includes(`${client} plugin marketplace add`), 'Repeat install should not re-add the marketplace')
    assert.ok(!repeat.stdout.includes(`${client} plugin add `) && !repeat.stdout.includes(`${client} plugin install `), 'Repeat install should not reinstall the native plugin')
    await record('repeat-install', repeat, 'success and no native mutations')
  } else {
    const installedOld = await packageVersion(oldTarball, 'previous plugin', npmPath, npmEnv, join(profile.profileRoot, 'previous-package'))
    assert.equal(installedOld.version, oldPackage.version)
    const first = await invokeInstaller(profile, { nodePath, packageRoot: installedOld.packageRoot, client, dataRoot: profile.dataRoot, label: 'previous-install' })
    assert.equal(first.status, 0, `${client} legacy install: ${first.stderr}`)
    await assertPayload(profile.dataRoot, oldPackage.version, 'cli/device-cli.mjs')
    await record('previous-install', first, 'success')

    const installedNew = await packageVersion(newTarball, 'current plugin', npmPath, npmEnv, join(profile.profileRoot, 'current-package'))
    assert.equal(installedNew.version, newPackage.version)
    if (name.startsWith('upgrade-')) {
      const upgraded = await invokeInstaller(profile, { nodePath, packageRoot: installedNew.packageRoot, client, dataRoot: profile.dataRoot, label: 'upgrade-install' })
      assert.equal(upgraded.status, 0, `${client} upgrade: ${upgraded.stderr}`)
      await assertPayload(profile.dataRoot, oldPackage.version, 'cli/device-cli.mjs')
      await assertPayload(profile.dataRoot, newPackage.version, 'cli/device-cli.mjs')
      await record('upgrade-install', upgraded, 'success; old and new payloads retained')
    } else {
      const failed = await invokeInstaller(profile, { nodePath, packageRoot: installedNew.packageRoot, client, dataRoot: profile.dataRoot, label: 'injected-failure', failOnce: true })
      assert.equal(failed.status, 1, `${client} injected failure should be reported by the installer`)
      assert.match(failed.stderr, /status 73/i, `${client} failure report should include the native exit status`)
      assert.match(failed.stderr, /marketplace registered-current; plugin not-installed/i, `${client} failure report should describe the verified partial state`)
      await assertPayload(profile.dataRoot, oldPackage.version, 'cli/device-cli.mjs')
      await assertPayload(profile.dataRoot, newPackage.version, 'cli/device-cli.mjs')
      const markerInfo = await lstat(profile.failMarker).catch(() => undefined)
      assert.ok(markerInfo?.isFile(), 'Failure marker must exist after marketplace mutation')
      assert.match(await readFile(profile.commandLog, 'utf8'), /injected-after-success exit=73/, 'Marketplace add must have succeeded before the wrapper injected failure')
      await record('injected-failure', failed, 'installer reports status 73 and verified partial state')

      const retry = await invokeInstaller(profile, { nodePath, packageRoot: installedNew.packageRoot, client, dataRoot: profile.dataRoot, label: 'failure-retry' })
      assert.equal(retry.status, 0, `${client} recovery retry: ${retry.stderr}`)
      await assertPayload(profile.dataRoot, oldPackage.version, 'cli/device-cli.mjs')
      await assertPayload(profile.dataRoot, newPackage.version, 'cli/device-cli.mjs')
      await record('failure-retry', retry, 'success; old and new payloads retained')
    }
  }
  summary.scenarios.push({ name, ...scenarioResult })
  await writeJson(join(outputRoot, 'results.json'), summary)
}

async function main() {
  if (process.platform !== 'linux') throw new Error('Native plugin acceptance requires Linux and bubblewrap.')
  const major = Number(process.versions.node.split('.')[0])
  if (major < 22) throw new Error(`Use Node.js 22 or newer; current runtime is ${process.version}.`)
  if (process.env.CODEX_HOME && resolve(process.env.CODEX_HOME) !== resolve(homedir(), '.codex')) {
    throw new Error('CODEX_HOME points outside the default user config path; refusing to risk touching a real Codex profile.')
  }
  if (process.env.CLAUDE_CONFIG_DIR && resolve(process.env.CLAUDE_CONFIG_DIR) !== resolve(homedir(), '.claude')) {
    throw new Error('CLAUDE_CONFIG_DIR points outside the default user config path; refusing to risk touching a real Claude profile.')
  }

  const home = await realpath(homedir())
  const codexConfig = join(home, '.codex')
  const claudeConfigPath = join(home, '.claude')
  const claudeJson = join(home, '.claude.json')
  await assertNativeMountTarget(codexConfig, 'directory')
  await assertNativeMountTarget(claudeConfigPath, 'directory')
  await assertNativeMountTarget(claudeJson, 'file')

  const codexSource = await assertExecutable(args.codex, 'Codex')
  const claudeBinary = await assertExecutable(args.claude, 'Claude Code')
  const newTarball = await realpath(resolve(args.tarball))
  let oldTarball = args.previousTarball ? await realpath(resolve(args.previousTarball)) : undefined
  for (const tarball of [newTarball, oldTarball].filter(Boolean)) {
    const info = await lstat(tarball)
    if (!info.isFile() || info.isSymbolicLink()) throw new Error(`Tarball must be a regular file: ${tarball}`)
  }

  const requestedOutput = resolve(args.output)
  await mkdir(requestedOutput, { recursive: true })
  const outputRoot = await realpath(requestedOutput)
  const runRoot = join(outputRoot, `native-plugin-acceptance-${new Date().toISOString().replaceAll(':', '').replaceAll('.', '')}-${process.pid}`)
  await mkdir(runRoot, { recursive: false })
  activeRunRoot = runRoot
  await mkdir(join(runRoot, 'bin'), { recursive: true })
  await mkdir(join(runRoot, 'tmp'), { recursive: true })
  await mkdir(join(runRoot, 'npm-cache'), { recursive: true })

  const codexCopy = join(runRoot, 'bin', 'codex')
  await copyFile(codexSource, codexCopy)
  await chmod(codexCopy, 0o755)
  const nodePath = await realpath(process.execPath)
  const npmPath = join(dirname(nodePath), 'npm')
  await assertExecutable(npmPath, 'npm')
  const npmEnv = {
    ...process.env,
    PATH: [dirname(nodePath), process.env.PATH ?? ''].filter(Boolean).join(':'),
    npm_config_cache: join(runRoot, 'npm-cache'),
    npm_config_update_notifier: 'false',
    TMPDIR: join(runRoot, 'tmp'),
    TMP: join(runRoot, 'tmp'),
    TEMP: join(runRoot, 'tmp'),
  }

  const bwrapPath = await findExecutable('bwrap')
  if (!bwrapPath) throw new Error('Linux bubblewrap (bwrap) must be installed and available on PATH.')
  const version = run(bwrapPath, ['--version'], { env: npmEnv })
  await writeJson(join(runRoot, 'bubblewrap-version.json'), version)
  assertCommandSucceeded(version, 'bubblewrap --version')
  const claudeConfig = { path: claudeConfigPath, binary: claudeBinary }
  const summary = {
    startedAt: new Date().toISOString(),
    node: process.version,
    bubblewrap: version.stdout.trim(),
    tarballs: { current: newTarball, previous: oldTarball, syntheticPrevious: !args.previousTarball },
    clients: { codex: codexSource, codexCopiedTo: codexCopy, claude: claudeBinary },
    isolatedConfigTargets: { codex: codexConfig, claude: claudeConfigPath, claudeJson },
    scenarios: [],
  }
  activeSummary = summary
  await writeJson(join(runRoot, 'results.json'), summary)
  const newPackage = await packageVersion(newTarball, 'current plugin', npmPath, npmEnv, join(runRoot, 'probe-current'))
  if (!oldTarball) oldTarball = await syntheticPreviousTarball(newPackage.packageRoot, runRoot, npmPath, npmEnv)
  summary.tarballs.previous = oldTarball
  const oldPackage = await packageVersion(oldTarball, 'previous plugin', npmPath, npmEnv, join(runRoot, 'probe-previous'))
  assert.notEqual(newPackage.version, oldPackage.version, 'current tarball version')

  summary.tarballs.currentVersion = newPackage.version
  summary.tarballs.previousVersion = oldPackage.version
  await writeJson(join(runRoot, 'results.json'), summary)
  const common = { runRoot, codexConfig, claudeConfig, claudeJson, nodePath, outputRoot: runRoot, npmEnv, npmPath, newTarball, oldTarball, newPackage, oldPackage, summary, bwrapPath }
  for (const client of ['codex', 'claude']) {
    await scenario({ ...common, name: `first-${client}`, client })
    await scenario({ ...common, name: `upgrade-${client}`, client })
    await scenario({ ...common, name: `recovery-${client}`, client })
  }
  summary.completedAt = new Date().toISOString()
  summary.passed = true
  await writeJson(join(runRoot, 'results.json'), summary)
  console.log(`Native plugin acceptance passed. Results: ${join(runRoot, 'results.json')}`)
}

main().catch(async error => {
  if (activeRunRoot && activeSummary) {
    activeSummary.completedAt = new Date().toISOString()
    activeSummary.passed = false
    activeSummary.failure = error.message
    await writeJson(join(activeRunRoot, 'results.json'), activeSummary).catch(() => {})
  }
  console.error(`Native plugin acceptance failed: ${error.stack ?? error.message}`)
  process.exitCode = 1
})

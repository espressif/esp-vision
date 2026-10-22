#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { createHash, randomBytes } from 'node:crypto'
import { spawn } from 'node:child_process'
import { mkdir, readFile, realpath, writeFile } from 'node:fs/promises'
import { basename, isAbsolute, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const fixturePath = fileURLToPath(new URL('./fixtures/camera_lcd.py', import.meta.url))
const persistentFixturePath = fileURLToPath(new URL('./fixtures/persistent_camera_lcd.py', import.meta.url))
const DEFAULT_WATCH_MS = 3500
const PERSISTENT_WATCH_MS = 3500
const PERSISTENT_STARTUP_TIMEOUT_MS = 15000
const PERSISTENT_STOP_SCREEN_TIMEOUT_MS = 8000
const PERSISTENT_STOP_GRACE_MS = 9000
const COMMAND_TIMEOUT_MS = 45000
const MAX_JPEG_BYTES = 2 * 1024 * 1024
const TARGET_WARNING = 'Target acceptance requires a dedicated ESP32_P4X_EYE board and can interrupt an existing device application; a fresh SDK runtime snapshot does not prove the target is idle.'

function usage() {
  return `Usage: node target_cli_acceptance.mjs --cli ABS_BUILT_CLI --port PORT --output DIR [--python interpreter]\n\nWARNING: ${TARGET_WARNING}`
}

function argumentsFrom(argv) {
  const options = {}
  for (let index = 0; index < argv.length; index++) {
    const key = argv[index]
    if (key === '--help' || key === '-h') {
      options.help = true
      continue
    }
    if (!['--cli', '--port', '--output', '--python'].includes(key)) throw new Error(`Unknown argument: ${key}`)
    if (options[key.slice(2)] !== undefined || argv[index + 1] === undefined || argv[index + 1].startsWith('--')) {
      throw new Error(`Expected one value after ${key}`)
    }
    options[key.slice(2)] = argv[++index]
  }
  if (options.help) return options
  for (const key of ['cli', 'port', 'output']) {
    if (!options[key]) throw new Error(`Missing required ${key === 'cli' ? '--cli' : `--${key}`}`)
  }
  if (!isAbsolute(options.cli)) throw new Error('--cli must be an absolute path to a built CLI entry point')
  if (!isAbsolute(options.output)) throw new Error('--output must be an absolute path')
  if (!options.port.trim()) throw new Error('--port must not be empty')
  return options
}

function parseJsonStdout(stdout, label) {
  const text = stdout.trim()
  assert.ok(text, `${label}: expected one JSON document on stdout`)
  assert.equal(text.split(/\r?\n/).length, 1, `${label}: stdout must contain exactly one JSON line`)
  try { return JSON.parse(text) }
  catch (cause) { throw new Error(`${label}: stdout was not valid JSON: ${cause.message}`) }
}

function parseNdjsonStderr(stderr, label) {
  const lines = stderr.split(/\r?\n/).filter(line => line.length > 0)
  return lines.map((line, index) => {
    try { return JSON.parse(line) }
    catch (cause) { throw new Error(`${label}: stderr line ${index + 1} was not JSON: ${cause.message}`) }
  })
}

function jpegDimensions(data, label) {
  assert.ok(data.length >= 4 && data[0] === 0xff && data[1] === 0xd8, `${label}: missing JPEG SOI marker`)
  const sof = new Set([0xc0, 0xc1, 0xc2, 0xc3, 0xc5, 0xc6, 0xc7, 0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf])
  let offset = 2
  while (offset < data.length) {
    while (offset < data.length && data[offset] !== 0xff) offset++
    while (offset < data.length && data[offset] === 0xff) offset++
    if (offset >= data.length) break
    const marker = data[offset++]
    if (marker === 0xd9 || marker === 0xda) break
    if (marker === 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue
    assert.ok(offset + 2 <= data.length, `${label}: truncated JPEG segment length`)
    const length = data.readUInt16BE(offset)
    assert.ok(length >= 2 && offset + length <= data.length, `${label}: malformed JPEG segment`)
    if (sof.has(marker)) {
      assert.ok(length >= 7, `${label}: malformed JPEG frame header`)
      return { height: data.readUInt16BE(offset + 3), width: data.readUInt16BE(offset + 5) }
    }
    offset += length
  }
  throw new Error(`${label}: no JPEG frame dimensions found`)
}

function isEvent(event, name, state) {
  return event?.event === name && (state === undefined || event.state === state)
}

async function main() {
  const options = argumentsFrom(process.argv.slice(2))
  if (options.help) {
    process.stdout.write(`${usage()}\n`)
    return
  }
  if (Number(process.versions.node.split('.')[0]) < 22) throw new Error('Run this acceptance with Node.js 22 or newer')
  process.stderr.write(`WARNING: ${TARGET_WARNING}\n`)
  const cli = await realpath(options.cli)
  const outputRoot = resolve(options.output)
  await mkdir(outputRoot, { recursive: true })
  const token = `${Date.now().toString(36)}_${process.pid.toString(36)}_${randomBytes(4).toString('hex')}`
  const runDir = join(outputRoot, `target-cli-${token}`)
  const tempDir = join(runDir, 'tmp')
  await mkdir(tempDir, { recursive: true })
  const env = { ...process.env, TMPDIR: tempDir, TMP: tempDir, TEMP: tempDir }
  const commonArgs = ['--port', options.port, ...(options.python ? ['--python', options.python] : []), '--json']
  const deviceDir = `/.espvision_cli_acceptance_${token}`
  const deviceBlob = `${deviceDir}/payload.bin`
  const deviceBatchDir = `${deviceDir}/batch`
  const deviceVmLoopApp = `${deviceDir}/vm-loop-hook.py`
  const devicePersistentApp = `${deviceDir}/persistent_camera_lcd.py`
  const devicePersistentStop = `${deviceDir}/stop.txt`
  const devicePersistentAck = `${deviceDir}/stop-observed.txt`
  const devicePersistentBoot = `${deviceDir}/boot-count.txt`
  const devicePersistentProgress = `${deviceDir}/progress.txt`
  const devicePersistentDone = `${deviceDir}/done.txt`
  const persistentStopToken = `stop_${token}`
  const persistentStopLocal = join(runDir, 'persistent-stop.txt')
  const persistentAckLocal = join(runDir, 'persistent-stop-observed.txt')
  const persistentBootLocal = join(runDir, 'persistent-boot-count.txt')
  const persistentProgressLocal = join(runDir, 'persistent-progress.txt')
  const persistentDoneLocal = join(runDir, 'persistent-done.txt')
  const checks = []
  const artifacts = {}
  let commandNumber = 0
  let ownedDir = false
  let ownedBlob = false
  let ownedBatchDir = false
  let ownedVmLoopApp = false
  let ownedPersistentApp = false
  let ownedPersistentStop = false
  let ownedPersistentAck = false
  let ownedPersistentBoot = false
  let ownedPersistentProgress = false
  let ownedPersistentDone = false
  let persistentAppMayBeRunning = false
  let persistentAppStopped = false
  let persistentStopWritten = false
  let vmLoopMayBeRunning = false
  let vmLoopStopped = false
  let primaryFailure

  function assertNoPythonFailed(result, label) {
    const failure = result.events.find(event => event.event === 'python.failed')
    assert.equal(failure, undefined, `${label}: intentional disconnect must not report python.failed`)
  }

  const record = () => writeFile(join(runDir, 'report.json'), `${JSON.stringify({
    schemaVersion: 1,
    port: options.port,
    cli: basename(cli),
    targetAcceptance: {
      dedicatedBoardRequired: true,
      mayInterruptExistingApplication: true,
      idleCannotBeInferredFromFreshSdkRuntime: true,
    },
    checks,
    artifacts,
    ...(primaryFailure ? { failure: { message: primaryFailure.message, stack: primaryFailure.stack, ...(primaryFailure.cleanupError ? { cleanupError: primaryFailure.cleanupError } : {}) } } : {}),
  }, null, 2)}\n`)

  async function invoke(label, args, { timeoutMs = COMMAND_TIMEOUT_MS, onEvent } = {}) {
    const number = String(++commandNumber).padStart(2, '0')
    const safeLabel = label.replace(/[^a-z0-9-]+/gi, '-').toLowerCase()
    const stdoutPath = join(runDir, `${number}-${safeLabel}.stdout.json`)
    const stderrPath = join(runDir, `${number}-${safeLabel}.stderr.ndjson`)
    const fullArgs = [cli, ...args, ...commonArgs]
    let stdout = ''
    let stderr = ''
    let pendingLine = ''
    const events = []
    let callbackFailure
    let watchdogExpired = false
    const child = spawn(process.execPath, fullArgs, { cwd: runDir, env, stdio: ['ignore', 'pipe', 'pipe'] })
    let exited = false
    let watchdog
    const exitPromise = new Promise((resolveExit, rejectExit) => {
      child.once('error', rejectExit)
      child.once('close', (code, signal) => {
        exited = true
        resolveExit({ code, signal })
      })
    })
    child.stdout.setEncoding('utf8').on('data', chunk => { stdout += chunk })
    child.stderr.setEncoding('utf8').on('data', chunk => {
      stderr += chunk
      pendingLine += chunk
      const complete = pendingLine.split(/\r?\n/)
      pendingLine = complete.pop() ?? ''
      for (const line of complete) {
        if (!line) continue
        let event
        try { event = JSON.parse(line) }
        catch (cause) {
          callbackFailure ??= new Error(`${label}: stderr emitted non-NDJSON output: ${cause.message}`)
          child.kill('SIGKILL')
          continue
        }
        events.push(event)
        try { onEvent?.(event, child) }
        catch (cause) { callbackFailure ??= cause; child.kill('SIGKILL') }
      }
    })
    watchdog = setTimeout(() => {
      if (!exited) {
        watchdogExpired = true
        child.kill('SIGKILL')
      }
    }, timeoutMs)
    const exit = await exitPromise.finally(() => clearTimeout(watchdog))
    if (pendingLine.trim()) {
      try { events.push(JSON.parse(pendingLine)) }
      catch (cause) { callbackFailure ??= new Error(`${label}: final stderr line was not NDJSON: ${cause.message}`) }
    }
    await writeFile(stdoutPath, stdout)
    await writeFile(stderrPath, stderr)
    artifacts[label] = { stdout: stdoutPath, stderr: stderrPath, exitCode: exit.code, signal: exit.signal }
    if (watchdogExpired) throw new Error(`${label}: subprocess exceeded its ${timeoutMs}ms watchdog`)
    if (callbackFailure) throw callbackFailure
    const envelope = parseJsonStdout(stdout, label)
    parseNdjsonStderr(stderr, label)
    return { ...exit, envelope, events, stdout, stderr }
  }

  function succeeded(result, label) {
    assert.equal(result.code, 0, `${label}: expected exit code 0, got ${result.code} (${result.signal ?? 'no signal'})`)
    assert.equal(result.envelope.schemaVersion, 1, `${label}: unexpected schema version`)
    assert.equal(result.envelope.ok, true, `${label}: expected a successful JSON envelope`)
    return result.envelope.result
  }

  function failed(result, code, label) {
    assert.equal(result.code, 1, `${label}: expected ordinary failure exit code 1, got ${result.code}`)
    assert.equal(result.envelope.schemaVersion, 1, `${label}: unexpected schema version`)
    assert.equal(result.envelope.ok, false, `${label}: expected a failure JSON envelope`)
    assert.equal(result.envelope.error?.code, code, `${label}: unexpected error code`)
    return result.envelope
  }

  async function check(name, fn) {
    const started = Date.now()
    try {
      const details = await fn()
      checks.push({ name, ok: true, durationMs: Date.now() - started, ...(details ?? {}) })
      await record()
      process.stderr.write(`PASS ${name}\n`)
      return details
    } catch (error) {
      checks.push({ name, ok: false, durationMs: Date.now() - started, error: error.message })
      if (error.cleanupError) checks.at(-1).cleanupError = error.cleanupError
      await record()
      throw error
    }
  }

  async function reconnect(label) {
    const result = await invoke(label, ['python', '--source', `print('${label.replace(/[^a-z0-9_-]/gi, '_')}')`])
    const metadata = succeeded(result, label)
    assert.ok(result.events.some(event => event.event === 'python.output' && String(event.data).includes(label.replace(/[^a-z0-9_-]/gi, '_'))), `${label}: no Python output event after reconnect`)
    return { durationMs: metadata.durationMs }
  }

  async function sendPersistentStopSignal(label) {
    if (persistentStopWritten) return
    await writeFile(persistentStopLocal, persistentStopToken)
    ownedPersistentStop = true
    const result = await invoke(label, ['files', 'write', devicePersistentStop, '--file', persistentStopLocal], { timeoutMs: 20000 })
    const metadata = succeeded(result, label)
    assert.equal(metadata.bytes, Buffer.byteLength(persistentStopToken), `${label}: control signal byte count mismatch`)
    persistentStopWritten = true
  }

  async function stopPersistentAppDuringCleanup(failures) {
    if (!persistentAppMayBeRunning || persistentAppStopped) return
    try {
      await sendPersistentStopSignal('cleanup-write-persistent-stop-signal')
      await new Promise(resolveDelay => setTimeout(resolveDelay, PERSISTENT_STOP_GRACE_MS + 1500))
      const result = await invoke('cleanup-read-persistent-done', ['files', 'read', devicePersistentDone, '--output', persistentDoneLocal, '--max-bytes', '128'])
      const metadata = succeeded(result, 'cleanup-read-persistent-done')
      const doneBytes = await readFile(persistentDoneLocal)
      const done = doneBytes.toString('utf8').trim()
      assert.ok(done.includes('cleanup=ok'), 'persistent fixture did not record successful cleanup')
      assert.equal(metadata.bytes, doneBytes.length, 'persistent cleanup marker byte metadata mismatch')
      persistentAppStopped = true
      return
    } catch (error) {
      failures.push(`persistent app graceful stop: ${error.message}`)
    }

    try {
      const stopped = await invoke('cleanup-control-stop-persistent-app', ['control', '--action', 'stop'], { timeoutMs: 15000 })
      succeeded(stopped, 'cleanup-control-stop-persistent-app')
      persistentAppStopped = true
    } catch (error) {
      failures.push(`persistent app fallback stop: ${error.message}`)
    }
  }

  async function cleanupRemote() {
    const failures = []
    if (vmLoopMayBeRunning && !vmLoopStopped) {
      try {
        const stopped = await invoke('cleanup-control-stop-bytecode-loop', ['control', '--action', 'stop'], { timeoutMs: 15000 })
        succeeded(stopped, 'cleanup-control-stop-bytecode-loop')
        vmLoopStopped = true
        vmLoopMayBeRunning = false
      } catch (error) { failures.push(`bytecode loop stop: ${error.message}`) }
    }
    await stopPersistentAppDuringCleanup(failures)
    for (const [exists, path, type] of [
      [ownedBatchDir, deviceBatchDir, 'directory'],
      [ownedVmLoopApp, deviceVmLoopApp, 'file'],
      [ownedPersistentStop, devicePersistentStop, 'file'],
      [ownedPersistentAck, devicePersistentAck, 'file'],
      [ownedPersistentBoot, devicePersistentBoot, 'file'],
      [ownedPersistentProgress, devicePersistentProgress, 'file'],
      [ownedPersistentDone, devicePersistentDone, 'file'],
      [ownedPersistentApp, devicePersistentApp, 'file'],
      [ownedBlob, deviceBlob, 'file'],
      [ownedDir, deviceDir, 'directory'],
    ]) {
      if (!exists) continue
      try {
        const result = await invoke(`cleanup-${type}-${basename(path)}`, ['files', 'remove', path, '--type', type])
        if (result.envelope.ok === false && ['NOT_FOUND', 'FILE_NOT_FOUND'].includes(result.envelope.error?.code)) continue
        succeeded(result, `cleanup-${type}-${basename(path)}`)
      } catch (error) { failures.push(`${path}: ${error.message}`) }
    }
    if (failures.length) throw new Error(`Failed to remove acceptance-owned device files: ${failures.join('; ')}`)
  }

  try {
    await check('preflight-ready-esp32-p4x-eye', async () => {
      const result = await invoke('preflight-inspect', ['inspect', '--fields', 'identity,capabilities,runtime'])
      const inspection = succeeded(result, 'preflight-inspect')
      assert.equal(inspection.identity?.board, 'ESP32_P4X_EYE', 'acceptance requires an ESP32_P4X_EYE board')
      assert.equal(inspection.runtime?.state, 'ready', 'device VM is not ready')
      assert.equal(inspection.runtime?.vmReady, true, 'device VM readiness flag is not true')
      const features = inspection.capabilities?.features ?? []
      assert.ok(features.includes('debug.capture_frame'), 'P4 camera capture is unsupported by the device')
      assert.ok(features.includes('debug.capture_screen'), 'P4 screen capture is unsupported by the device')
      return {
        board: inspection.identity.board,
        vmReady: true,
        dedicatedBoardRequired: true,
        mayInterruptExistingApplication: true,
        idleCannotBeInferredFromFreshSdkRuntime: true,
      }
    })

    await check('silent-python-over-default-timeout', async () => {
      const script = join(runDir, 'silent-31s.py')
      await writeFile(script, "import time\ntime.sleep_ms(31000)\nprint('silent-31s-complete')\n")
      const result = await invoke('silent-python-31s', ['python', '--file', script], { timeoutMs: 50000 })
      const summary = succeeded(result, 'silent-python-31s')
      assert.ok(summary.durationMs >= 30000, `expected >30s Python run, got ${summary.durationMs}ms`)
      assert.ok(result.events.some(event => event.event === 'python.output' && String(event.data).includes('silent-31s-complete')), 'Python completion output was not streamed')
      assert.equal(JSON.parse(result.stdout).command, 'python')
      return { cliDurationMs: summary.durationMs, timeoutOmitted: true }
    })

    await check('explicit-timeout-and-reconnect', async () => {
      const script = join(runDir, 'timeout.py')
      await writeFile(script, 'while True:\n    pass\n')
      const result = await invoke('python-timeout-3s', ['python', '--file', script, '--timeout', '3000'], { timeoutMs: 15000 })
      const failure = failed(result, 'TIMEOUT', 'python-timeout-3s')
      assert.ok(result.events.some(event => isEvent(event, 'state', 'disconnected')), 'timeout did not report disconnect')
      const reconnected = await reconnect('reconnect-after-timeout')
      return { error: failure.error.code, reconnectDurationMs: reconnected.durationMs }
    })

    await check('sigint-cancellation-and-reconnect', async () => {
      const script = join(runDir, 'cancel.py')
      await writeFile(script, 'while True:\n    pass\n')
      let signalSent = false
      const result = await invoke('python-sigint-cancel', ['python', '--file', script], {
        timeoutMs: 20000,
        onEvent(event, child) {
          if (!signalSent && isEvent(event, 'state', 'connected')) {
            signalSent = true
            setTimeout(() => child.kill('SIGINT'), 300)
          }
        },
      })
      assert.equal(signalSent, true, 'SIGINT test never observed a connected device')
      assert.equal(result.code, 130, `SIGINT should exit 130, got ${result.code}`)
      assert.equal(result.envelope.ok, false, 'SIGINT should produce a failure envelope')
      assert.equal(result.envelope.error?.code, 'CANCELLED', 'SIGINT should report CANCELLED')
      assert.ok(result.events.some(event => isEvent(event, 'state', 'disconnected')), 'SIGINT did not report disconnect')
      const reconnected = await reconnect('reconnect-after-sigint')
      return { exitCode: result.code, reconnectDurationMs: reconnected.durationMs }
    })

    const payload = Buffer.alloc(56 * 1024)
    for (let index = 0; index < payload.length; index++) payload[index] = (index * 31 + 17) & 0xff
    const payloadPath = join(runDir, 'payload-56k.bin')
    const downloadedPath = join(runDir, 'payload-56k-downloaded.bin')
    await writeFile(payloadPath, payload)
    const expectedHash = createHash('sha256').update(payload).digest('hex')

    await check('owned-binary-files-roundtrip-56k', async () => {
      let result = await invoke('mkdir-device-owned-prefix', ['files', 'mkdir', deviceDir])
      succeeded(result, 'mkdir-device-owned-prefix')
      ownedDir = true
      result = await invoke('write-device-payload-56k', ['files', 'write', deviceBlob, '--file', payloadPath])
      ownedBlob = true
      const written = succeeded(result, 'write-device-payload-56k')
      assert.equal(written.bytes, payload.length, 'write metadata byte count mismatch')
      result = await invoke('read-device-payload-56k', ['files', 'read', deviceBlob, '--output', downloadedPath, '--max-bytes', String(payload.length)])
      const read = succeeded(result, 'read-device-payload-56k')
      assert.equal(read.bytes, payload.length, 'read metadata byte count mismatch')
      const downloaded = await readFile(downloadedPath)
      assert.deepEqual(downloaded, payload, 'downloaded file bytes differ')
      assert.equal(createHash('sha256').update(downloaded).digest('hex'), expectedHash, 'downloaded SHA-256 differs')
      assert.ok(JSON.stringify(result.envelope).length < 4096, 'binary payload was unexpectedly embedded in JSON')
      return { bytes: payload.length, sha256: expectedHash }
    })

    await check('batch-one-connection-stops-at-first-failure', async () => {
      const batchPlan = join(runDir, 'partial-failure-plan.json')
      const shouldNotRun = 'cli-acceptance-batch-step-after-failure'
      const plan = {
        schemaVersion: 1,
        operations: [
          { op: 'python', source: "print('cli-acceptance-batch-step-one')" },
          { op: 'files.mkdir', path: deviceBatchDir },
          { op: 'files.read', path: deviceBlob, output: './unexpected-overread.bin', maxBytes: 1 },
          { op: 'python', source: `print('${shouldNotRun}')` },
        ],
      }
      await writeFile(batchPlan, `${JSON.stringify(plan, null, 2)}\n`)
      const result = await invoke('batch-first-failure', ['batch', batchPlan], { timeoutMs: 25000 })
      const report = result.envelope
      assert.equal(result.code, 1, `batch-first-failure: expected exit code 1, got ${result.code}`)
      assert.equal(report.schemaVersion, 1, 'batch-first-failure: unexpected schema version')
      assert.equal(report.ok, false, 'batch-first-failure: expected failure envelope')
      assert.equal(report.command, 'batch', 'batch-first-failure: command mismatch')
      ownedBatchDir = report.completed?.some(entry => entry.index === 2) === true
        || (report.failed?.index === 2 && report.failed?.operation === 'files.mkdir')
      assert.equal(report.completed?.length, 2, 'batch should report the two completed operations')
      assert.equal(report.failed?.index, 3, 'batch should fail at operation 3')
      assert.equal(report.failed?.error?.code, 'DEVICE_FILE_TOO_LARGE', 'batch failure code mismatch')
      assert.equal(report.cleanupError, undefined, 'batch disconnect cleanup reported an error')
      assert.ok(!result.events.some(event => String(event.data).includes(shouldNotRun)), 'batch ran a later operation after failure')
      assert.equal(result.events.filter(event => isEvent(event, 'state', 'connecting')).length, 1, 'batch should connect exactly once')
      assert.equal(result.events.filter(event => isEvent(event, 'state', 'connected')).length, 1, 'batch should establish exactly one connection')
      assert.equal(result.events.filter(event => isEvent(event, 'state', 'disconnected')).length, 1, 'batch should release its one connection')
      assert.ok(result.events.some(event => event.event === 'python.output' && String(event.data).includes('cli-acceptance-batch-step-one')), 'batch Python output was not streamed')
      return { completedOperations: report.completed.length, failedIndex: report.failed.index, connectionCount: 1 }
    })

    await check('watch-holds-port-and-releases-after-duration', async () => {
      let watchConnectedResolve
      const watchConnected = new Promise(resolveConnected => { watchConnectedResolve = resolveConnected })
      const watch = invoke('watch-port-ownership', ['watch', '--duration', String(DEFAULT_WATCH_MS)], {
        timeoutMs: 15000,
        onEvent(event) { if (isEvent(event, 'state', 'connected')) watchConnectedResolve() },
      })
      let connectionWatchdog
      let connectionError
      try {
        await Promise.race([
          watchConnected,
          new Promise((_, reject) => { connectionWatchdog = setTimeout(() => reject(new Error('watch did not connect')), 10000) }),
        ])
      } catch (error) { connectionError = error }
      finally { clearTimeout(connectionWatchdog) }
      if (connectionError) {
        await watch.catch(() => {})
        throw connectionError
      }
      let contenderError
      let busy
      let watchResult
      try {
        const contender = await invoke('watch-busy-contender', ['python', '--source', "print('busy-contender-must-not-run')"], { timeoutMs: 12000 })
        busy = failed(contender, 'SERIAL_BUSY', 'watch-busy-contender')
        assert.ok(!contender.events.some(event => event.event === 'python.output'), 'busy command unexpectedly ran Python')
      } catch (error) { contenderError = error }
      try { watchResult = await watch }
      catch (error) { contenderError ??= error }
      if (contenderError) throw contenderError
      const summary = succeeded(watchResult, 'watch-port-ownership')
      assert.ok(summary.durationMs >= DEFAULT_WATCH_MS - 200, `watch ended early (${summary.durationMs}ms)`)
      assert.ok(watchResult.events.some(event => isEvent(event, 'state', 'disconnected')), 'watch did not report release')
      const reconnected = await reconnect('reconnect-after-watch')
      return { busyCode: busy.error.code, watchDurationMs: summary.durationMs, reconnectDurationMs: reconnected.durationMs }
    })

    await check('camera-lcd-rainbow-and-screen-capture-cleanup', async () => {
      let fixtureSucceeded = false
      let primary
      let cleanupError
      let fixtureDurationMs
      let screenDetails
      let cameraDetails
      let vmLoopScreenDetails
      try {
        const result = await invoke('camera-lcd-fixture', ['python', '--file', fixturePath, '--timeout', '20000'], { timeoutMs: 30000 })
        const summary = succeeded(result, 'camera-lcd-fixture')
        fixtureSucceeded = true
        fixtureDurationMs = summary.durationMs
        assert.ok(result.events.some(event => event.event === 'python.output' && String(event.data).includes('ESP_VISION_LCD_ACCEPTANCE_FRAME')), 'camera/LCD fixture did not report its rendered frame')
        assert.ok(result.events.some(event => event.event === 'python.output' && String(event.data).includes('esp-vision')), 'fixture output does not confirm the requested label')

        const screenPath = join(runDir, 'screen.jpg')
        const capture = await invoke('capture-screen', ['capture', 'screen', '--output', screenPath, '--quality', '85', '--max-bytes', String(MAX_JPEG_BYTES)], { timeoutMs: 25000 })
        const metadata = succeeded(capture, 'capture-screen')
        const image = await readFile(screenPath)
        const dimensions = jpegDimensions(image, 'screen capture')
        assert.equal(metadata.bytes, image.length, 'screen byte metadata mismatch')
        assert.equal(metadata.source, 'screen', 'screen source metadata mismatch')
        assert.equal(metadata.mimeType, 'image/jpeg', 'screen MIME metadata mismatch')
        assert.equal(metadata.metadata?.width, dimensions.width, 'screen width metadata mismatch')
        assert.equal(metadata.metadata?.height, dimensions.height, 'screen height metadata mismatch')
        assert.ok(dimensions.width > 0 && dimensions.height > 0, 'screen JPEG has invalid dimensions')
        artifacts.screenImage = screenPath
        screenDetails = { bytes: image.length, ...dimensions }

        const cameraPath = join(runDir, 'camera.jpg')
        const cameraCapture = await invoke('capture-camera', ['capture', 'camera', '--output', cameraPath, '--quality', '85', '--max-bytes', String(MAX_JPEG_BYTES)], { timeoutMs: 25000 })
        const cameraMetadata = succeeded(cameraCapture, 'capture-camera')
        const cameraImage = await readFile(cameraPath)
        const cameraDimensions = jpegDimensions(cameraImage, 'camera capture')
        assert.ok(cameraDimensions.width > 0 && cameraDimensions.height > 0, 'camera JPEG has invalid dimensions')
        assert.equal(cameraMetadata.bytes, cameraImage.length, 'camera byte metadata mismatch')
        assert.equal(cameraMetadata.source, 'camera', 'camera source metadata mismatch')
        assert.equal(cameraMetadata.mimeType, 'image/jpeg', 'camera MIME metadata mismatch')
        assert.equal(cameraMetadata.metadata?.width, cameraDimensions.width, 'camera width metadata mismatch')
        assert.equal(cameraMetadata.metadata?.height, cameraDimensions.height, 'camera height metadata mismatch')
        artifacts.cameraImage = cameraPath
        cameraDetails = { bytes: cameraImage.length, ...cameraDimensions }

        const vmLoopSourcePath = join(runDir, 'vm-loop-hook.py')
        await writeFile(vmLoopSourcePath, 'counter = 0\nwhile True:\n    counter += 1\n    if counter == 1000:\n        counter = 0\n')
        artifacts.vmLoopSource = vmLoopSourcePath
        ownedVmLoopApp = true
        // Upload or start can fail after device-side work has begun; always
        // attempt an explicit control stop once this owned runner is attempted.
        vmLoopMayBeRunning = true
        const uploadLoop = await invoke('upload-bytecode-loop', ['files', 'write', deviceVmLoopApp, '--file', vmLoopSourcePath], { timeoutMs: 20000 })
        const uploadMetadata = succeeded(uploadLoop, 'upload-bytecode-loop')
        assert.equal(uploadMetadata.bytes, Buffer.byteLength(await readFile(vmLoopSourcePath)), 'bytecode loop upload byte count mismatch')

        // A rejected or timed-out acknowledgement can leave the runner active.
        const loopStart = await invoke('start-bytecode-loop', ['python', '--path', deviceVmLoopApp, '--completion', 'start'], { timeoutMs: 25000 })
        const loopStarted = succeeded(loopStart, 'start-bytecode-loop')
        assert.equal(loopStarted.started, true, 'tight bytecode loop start was not acknowledged')
        assert.equal(loopStarted.path, deviceVmLoopApp, 'tight bytecode loop start acknowledged the wrong path')
        assertNoPythonFailed(loopStart, 'start-bytecode-loop')

        const vmLoopScreenPath = join(runDir, 'screen-during-bytecode-loop.jpg')
        const vmLoopCapture = await invoke('capture-screen-during-bytecode-loop', ['capture', 'screen', '--output', vmLoopScreenPath, '--quality', '85', '--max-bytes', String(MAX_JPEG_BYTES)], { timeoutMs: 25000 })
        const vmLoopMetadata = succeeded(vmLoopCapture, 'capture-screen-during-bytecode-loop')
        const vmLoopImage = await readFile(vmLoopScreenPath)
        const vmLoopDimensions = jpegDimensions(vmLoopImage, 'screen capture during tight bytecode loop')
        assert.ok(vmLoopDimensions.width > 0 && vmLoopDimensions.height > 0, 'screen JPEG during tight bytecode loop has invalid dimensions')
        assert.equal(vmLoopMetadata.bytes, vmLoopImage.length, 'tight-loop screen byte metadata mismatch')
        assert.equal(vmLoopMetadata.source, 'screen', 'tight-loop screen source metadata mismatch')
        assert.equal(vmLoopMetadata.mimeType, 'image/jpeg', 'tight-loop screen MIME metadata mismatch')
        assert.equal(vmLoopMetadata.metadata?.width, vmLoopDimensions.width, 'tight-loop screen width metadata mismatch')
        assert.equal(vmLoopMetadata.metadata?.height, vmLoopDimensions.height, 'tight-loop screen height metadata mismatch')
        artifacts.screenDuringBytecodeLoop = vmLoopScreenPath
        vmLoopScreenDetails = { bytes: vmLoopImage.length, ...vmLoopDimensions }
      } catch (error) {
        primary = error
      } finally {
        const cleanupFailures = []
        if (vmLoopMayBeRunning && !vmLoopStopped) {
          try {
            const stop = await invoke('stop-bytecode-loop', ['control', '--action', 'stop'], { timeoutMs: 15000 })
            succeeded(stop, 'stop-bytecode-loop')
            vmLoopStopped = true
            vmLoopMayBeRunning = false
          } catch (error) { cleanupFailures.push(`control stop for tight bytecode loop: ${error.message}`) }
        }
        if (fixtureSucceeded) {
          const cleanupSource = `import sensor\n_cleanup_error = None\ntry:\n    sensor.shutdown()\nexcept BaseException as error:\n    _cleanup_error = error\ntry:\n    lcd.deinit()\nexcept BaseException as error:\n    if _cleanup_error is None:\n        _cleanup_error = error\nlcd = None\nif _cleanup_error is not None:\n    raise _cleanup_error\n`
          try {
            const cleanup = await invoke('cleanup-camera-lcd-fixture', ['python', '--source', cleanupSource], { timeoutMs: 15000 })
            succeeded(cleanup, 'cleanup-camera-lcd-fixture')
          } catch (error) { cleanupFailures.push(`sensor.shutdown/lcd.deinit: ${error.message}`) }
        }
        if (cleanupFailures.length) cleanupError = cleanupFailures.join('; ')
      }
      if (primary) {
        if (cleanupError) primary.cleanupError = cleanupError
        throw primary
      }
      if (cleanupError) throw Object.assign(new Error(`Camera/LCD cleanup failed: ${cleanupError}`), { cleanupError })
      return { fixtureDurationMs, screenCapture: screenDetails, cameraCapture: cameraDetails, tightLoopScreenCapture: vmLoopScreenDetails, tightLoopStopped: vmLoopStopped, cleanup: 'control stop, sensor.shutdown and lcd.deinit completed in host finally' }
    })

    await check('persistent-camera-lcd-survives-independent-cli-reconnects', async () => {
      const fixtureTemplate = await readFile(persistentFixturePath, 'utf8')
      const fixtureSource = fixtureTemplate
        .replaceAll('__CONTROL_PATH__', devicePersistentStop)
        .replaceAll('__OBSERVED_PATH__', devicePersistentAck)
        .replaceAll('__BOOT_PATH__', devicePersistentBoot)
        .replaceAll('__PROGRESS_PATH__', devicePersistentProgress)
        .replaceAll('__DONE_PATH__', devicePersistentDone)
        .replaceAll('__CONTROL_TOKEN__', persistentStopToken)
      assert.ok(!fixtureSource.includes('__CONTROL_') && !fixtureSource.includes('__OBSERVED_'), 'persistent fixture template was not fully configured')
      assert.ok(!fixtureSource.includes('__BOOT_') && !fixtureSource.includes('__PROGRESS_') && !fixtureSource.includes('__DONE_'), 'persistent fixture state paths were not fully configured')
      const fixtureLocalPath = join(runDir, 'persistent-camera-lcd.py')
      await writeFile(fixtureLocalPath, fixtureSource)

      ownedPersistentApp = true
      let result = await invoke('upload-persistent-camera-lcd-fixture', ['files', 'write', devicePersistentApp, '--file', fixtureLocalPath])
      const upload = succeeded(result, 'upload-persistent-camera-lcd-fixture')
      assert.equal(upload.bytes, Buffer.byteLength(fixtureSource), 'persistent fixture upload byte count mismatch')

      ownedPersistentAck = true
      ownedPersistentBoot = true
      ownedPersistentProgress = true
      ownedPersistentDone = true
      persistentAppMayBeRunning = true
      result = await invoke('start-persistent-camera-lcd-loop', ['python', '--path', devicePersistentApp, '--completion', 'start'], { timeoutMs: 25000 })
      const started = succeeded(result, 'start-persistent-camera-lcd-loop')
      assert.equal(started.started, true, 'persistent application start was not acknowledged')
      assert.equal(started.path, devicePersistentApp, 'persistent application start acknowledged the wrong device path')
      assertNoPythonFailed(result, 'start-persistent-camera-lcd-loop')

      result = await invoke('inspect-persistent-camera-lcd-reconnect', ['inspect', '--fields', 'identity,capabilities,runtime'])
      const inspection = succeeded(result, 'inspect-persistent-camera-lcd-reconnect')
      assert.equal(inspection.identity?.board, 'ESP32_P4X_EYE', 'reconnected to the wrong board')
      assert.equal(inspection.runtime?.state, 'ready', 'device VM was not ready after reconnect')
      assert.equal(inspection.runtime?.vmReady, true, 'device VM readiness flag was not true after reconnect')
      async function captureScreen(label, { displayReadyTimeoutMs = 0, timeoutMs = 25000 } = {}) {
        const imagePath = join(runDir, `${label}.jpg`)
        const deadline = Date.now() + displayReadyTimeoutMs
        let attempts = 0
        let capture
        let metadata
        while (true) {
          attempts += 1
          const attemptLabel = attempts === 1 ? label : `${label}-retry-${attempts}`
          if (displayReadyTimeoutMs > 0 && deadline - Date.now() < 2000) {
            throw new Error(`${label}: display remained not ready for ${displayReadyTimeoutMs}ms`)
          }
          const remainingMs = displayReadyTimeoutMs > 0 ? Math.max(1, deadline - Date.now()) : timeoutMs
          capture = await invoke(attemptLabel, ['capture', 'screen', '--output', imagePath, '--quality', '85', '--max-bytes', String(MAX_JPEG_BYTES)], { timeoutMs: Math.min(timeoutMs, remainingMs) })
          if (capture.envelope.ok) {
            metadata = succeeded(capture, attemptLabel)
            break
          }
          if (capture.envelope.error?.code !== 'DISPLAY_NOT_READY') succeeded(capture, attemptLabel)
          failed(capture, 'DISPLAY_NOT_READY', attemptLabel)
          if (Date.now() >= deadline) throw new Error(`${label}: display remained not ready for ${displayReadyTimeoutMs}ms`)
          await new Promise(resolveDelay => setTimeout(resolveDelay, 400))
        }
        const image = await readFile(imagePath)
        const dimensions = jpegDimensions(image, label)
        assert.equal(metadata.source, 'screen', `${label}: unexpected capture source`)
        assert.equal(metadata.mimeType, 'image/jpeg', `${label}: unexpected MIME type`)
        assert.equal(metadata.bytes, image.length, `${label}: byte metadata mismatch`)
        assert.equal(metadata.metadata?.width, dimensions.width, `${label}: width metadata mismatch`)
        assert.equal(metadata.metadata?.height, dimensions.height, `${label}: height metadata mismatch`)
        assert.ok(dimensions.width > 0 && dimensions.height > 0, `${label}: invalid image dimensions`)
        artifacts[label] = imagePath
        return { image, dimensions, output: imagePath, hash: createHash('sha256').update(image).digest('hex'), attempts, durationMs: metadata.durationMs }
      }

      const firstScreen = await captureScreen('persistent-screen-after-startup', { displayReadyTimeoutMs: PERSISTENT_STARTUP_TIMEOUT_MS })
      const watched = await invoke('watch-persistent-camera-lcd-output', ['watch', '--duration', String(PERSISTENT_WATCH_MS)], { timeoutMs: PERSISTENT_WATCH_MS + 15000 })
      const watchSummary = succeeded(watched, 'watch-persistent-camera-lcd-output')
      assert.ok(watchSummary.durationMs >= PERSISTENT_WATCH_MS - 250, 'passive persistent application watch ended early')
      assert.ok(watched.events.some(event => isEvent(event, 'state', 'disconnected')), 'passive watch did not report release')
      const secondScreen = await captureScreen('persistent-screen-after-passive-watch')
      assert.notEqual(secondScreen.hash, firstScreen.hash, 'LCD frame counter did not change across separate CLI capture processes')

      await sendPersistentStopSignal('write-persistent-loop-stop-signal')
      async function waitForStableStopScreen() {
        const deadline = Date.now() + PERSISTENT_STOP_SCREEN_TIMEOUT_MS
        let previousHash
        let attempts = 0
        while (Date.now() < deadline) {
          attempts += 1
          const remainingMs = deadline - Date.now()
          const current = await captureScreen(`persistent-screen-stop-poll-${attempts}`, { timeoutMs: Math.max(1, remainingMs) })
          if (current.hash !== secondScreen.hash && current.hash === previousHash) {
            artifacts.persistentStopState = current.output
            return { ...current, attempts }
          }
          previousHash = current.hash
          await new Promise(resolveDelay => setTimeout(resolveDelay, 350))
        }
        throw new Error(`LCD did not settle on its visible stop marker within ${PERSISTENT_STOP_SCREEN_TIMEOUT_MS}ms`)
      }

      const stopScreen = await waitForStableStopScreen()
      await new Promise(resolveDelay => setTimeout(resolveDelay, PERSISTENT_STOP_GRACE_MS + 1500))

      result = await invoke('read-persistent-loop-cleanup-state', ['files', 'read', devicePersistentDone, '--output', persistentDoneLocal, '--max-bytes', '128'])
      const doneMetadata = succeeded(result, 'read-persistent-loop-cleanup-state')
      const doneBytes = await readFile(persistentDoneLocal)
      const doneText = doneBytes.toString('utf8').trim()
      assert.equal(doneMetadata.bytes, doneBytes.length, 'persistent cleanup state byte metadata mismatch')
      persistentAppStopped = true
      const doneFields = Object.fromEntries(doneText.split(/\r?\n/).map(line => line.split('=')))
      assert.equal(doneFields.cleanup, 'ok', 'persistent app did not finish its finally cleanup')
      assert.equal(doneFields.stop_frame >= 0, true, 'persistent app never recorded observing its files.write signal')

      result = await invoke('read-persistent-loop-stop-observation', ['files', 'read', devicePersistentAck, '--output', persistentAckLocal, '--max-bytes', '96'])
      const ackMetadata = succeeded(result, 'read-persistent-loop-stop-observation')
      const ackText = (await readFile(persistentAckLocal, 'utf8')).trim()
      const ackMatch = ackText.match(/^(\S+) (\d+)$/)
      assert.ok(ackMatch, 'stop observation marker did not contain its token and frame count')
      assert.equal(ackMatch[1], persistentStopToken, 'stop observation marker belongs to a different acceptance run')
      assert.equal(Number(doneFields.stop_frame), Number(ackMatch[2]), 'cleanup and signal-observation files disagree on the stop frame')
      assert.equal(ackMetadata.bytes, Buffer.byteLength(ackText), 'stop observation marker byte metadata mismatch')

      result = await invoke('read-persistent-loop-boot-count', ['files', 'read', devicePersistentBoot, '--output', persistentBootLocal, '--max-bytes', '32'])
      const bootMetadata = succeeded(result, 'read-persistent-loop-boot-count')
      const bootText = (await readFile(persistentBootLocal, 'utf8')).trim()
      assert.equal(bootText, '1', `persistent fixture started ${bootText} times across CLI reconnects`)
      assert.equal(doneFields.boot, bootText, 'cleanup state and boot count file disagree')
      assert.equal(bootMetadata.bytes, Buffer.byteLength(bootText), 'boot count byte metadata mismatch')

      result = await invoke('read-persistent-loop-progress', ['files', 'read', devicePersistentProgress, '--output', persistentProgressLocal, '--max-bytes', '32'])
      const progressMetadata = succeeded(result, 'read-persistent-loop-progress')
      const progressText = (await readFile(persistentProgressLocal, 'utf8')).trim()
      const progressFrameCount = Number(progressText)
      assert.ok(Number.isSafeInteger(progressFrameCount) && progressFrameCount > Number(ackMatch[2]), 'final camera/LCD progress did not advance beyond signal observation')
      assert.equal(Number(doneFields.frames), progressFrameCount, 'cleanup and progress files disagree on the final frame count')
      assert.equal(progressMetadata.bytes, Buffer.byteLength(progressText), 'progress marker byte metadata mismatch')

      return {
        startAcknowledged: true,
        noFalsePythonFailedOnStartDisconnect: true,
        startupCaptureAttempts: firstScreen.attempts,
        liveScreenHashesDiffer: true,
        passiveWatchDurationMs: watchSummary.durationMs,
        stopMarkerScreenStable: true,
        stopMarkerScreenshot: stopScreen.output,
        stopSignalObservedAtFrame: Number(ackMatch[2]),
        bootCount: Number(bootText),
        finalProgressFrameCount: progressFrameCount,
        cleanup: 'persistent fixture recorded sensor.shutdown and lcd.deinit completion in finally',
      }
    })

    await cleanupRemote()
    await record()
    process.stdout.write(`${JSON.stringify({ ok: true, output: runDir, checks: checks.length, artifacts }, null, 2)}\n`)
  } catch (error) {
    primaryFailure = error
    try { await cleanupRemote() }
    catch (cleanupError) { primaryFailure.cleanupError = cleanupError.message }
    await record()
    process.stderr.write(`FAIL ${error.message}\nArtifacts retained in ${runDir}\n`)
    process.exitCode = 1
  }
}

main().catch(error => {
  process.stderr.write(`FAIL ${error.message}\n${usage()}\n`)
  process.exitCode = 2
})

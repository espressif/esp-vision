// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { mkdtemp, readFile, writeFile, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join, dirname } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import vm from 'node:vm'
import { webcrypto } from 'node:crypto'
import { build } from 'esbuild'

const root = dirname(dirname(fileURLToPath(import.meta.url)))
const temporary = await mkdtemp(join(tmpdir(), 'esp-vision-browser-'))
try {
  // Bundle the actual pinned dependency and its dynamic JSON stubs for browsers.
  const result = await build({
    stdin: { contents: `export { createDevice } from './src/index.js'; export { openBrowserRom } from './src/browser.js'; export { loadReleaseManifest } from './src/firmware.js'; export { ESPLoader as TestLoader } from 'tasmota-webserial-esptool'; export { browserRomCompatibility as testCompatibility } from './src/browser_rom_compat.js';`, resolveDir: root },
    bundle: true, platform: 'browser', format: 'iife', globalName: 'DeviceCore',
    write: false, metafile: true,
  })
  const code = result.outputFiles[0].text
  assert.ok(Object.keys(result.metafile.inputs).some(path => path.includes('tasmota-webserial-esptool/dist/esp_loader.js')))
  assert.ok(Object.keys(result.metafile.inputs).some(path => path.includes('stubs/esp32p4.json')))
  assert.ok(Object.keys(result.metafile.inputs).some(path => path.includes('stubs/esp32s31.json')))
  assert.ok(!Object.keys(result.metafile.inputs).some(path => /src\/(node|cli)/.test(path)))
  const context = vm.createContext({ EventTarget, URL, TextDecoder, TextEncoder, Uint8Array, AbortController, setTimeout, clearTimeout, console, crypto: webcrypto, structuredClone })
  vm.runInContext(code, context)
  let opens = 0, closes = 0
  const adapter = context.DeviceCore.openBrowserRom({ port: {
    async open() { opens++; throw new Error('real dependency reached SerialPort.open') },
    async close() { closes++ },
  } })
  assert.equal(opens, 0, 'import and construction must not open/reset the port')
  await assert.rejects(adapter.initialize(), /real dependency reached SerialPort.open/)
  assert.equal(opens, 1)
  // Run upstream's actual SLIP parser in both CDC and UART modes. A blank board
  // may emit a boot log before the stub greeting; invalid escapes must still fail.
  for (const cdc of [false, true]) {
    const raw = new context.DeviceCore.TestLoader({}, { log() {}, error() {}, debug() {} })
    context.DeviceCore.testCompatibility(raw)
    raw._isCDCDevice = cdc
    raw.__inputBuffer = []
    raw._inputBuffer.push(71, 97, 114, 0xc0, 79, 72, 65, 73, 0xc0)
    assert.deepEqual(Array.from(await raw.readPacket(100)), [79, 72, 65, 73])
    raw._clearInputBuffer()
    raw._inputBuffer.push(0xc0, 0xdb, 0x47, 0xc0)
    await assert.rejects(raw.readPacket(100), /Invalid SLIP escape/)
  }
  const prototype = context.DeviceCore.TestLoader.prototype
  const originals = Object.fromEntries(['initialize', 'disconnect', 'getSecurityInfo', 'runStub', 'hardReset'].map(key => [key, prototype[key]]))
  let initializeCalls = 0, disconnectCalls = 0
  prototype.disconnect = async function () { disconnectCalls++; await this.port.close() }
  const fakePort = () => ({
    async open() { this.readable = {}; this.writable = {} },
    async close() { this.readable = null; this.writable = null },
  })
  try {
    prototype.initialize = async () => { initializeCalls++ }
    const controller = new AbortController()
    const loader = context.DeviceCore.openBrowserRom({ port: fakePort(), signal: controller.signal })
    const first = loader.initialize()
    assert.equal(loader.initialize(), first, 'initialize must share one ROM connection')
    await first
    controller.abort()
    assert.equal(disconnectCalls, 0, 'abort after initialization must not interrupt flash/readback')
    await loader.disconnect(); await loader.disconnect()
    assert.equal(disconnectCalls, 1)
    assert.equal(initializeCalls, 1)
    prototype.initialize = async () => { throw new Error('sync failed') }
    await assert.rejects(context.DeviceCore.openBrowserRom({ port: fakePort() }).initialize(), /sync failed/)
    assert.equal(disconnectCalls, 2, 'failed ROM sync must release the acquired port')
    let finishOpen
    const waitingPort = fakePort()
    const open = waitingPort.open
    waitingPort.open = async function () {
      await new Promise(resolve => { finishOpen = resolve })
      await open.call(this)
    }
    const cancelled = new AbortController()
    const late = context.DeviceCore.openBrowserRom({ port: waitingPort, signal: cancelled.signal })
    const starting = late.initialize()
    while (!finishOpen) await new Promise(resolve => setTimeout(resolve, 1))
    cancelled.abort()
    await assert.rejects(starting, { name: 'AbortError' })
    finishOpen()
    await new Promise(resolve => setTimeout(resolve, 5))
    assert.equal(disconnectCalls, 3, 'an open completing after cancellation must close')
    assert.equal(waitingPort.readable, null)
    const data = new Uint8Array([0xe9, 1, 2, 3]), table = new Uint8Array([4, 5])
    const hash = async bytes => Array.from(new Uint8Array(await webcrypto.subtle.digest('SHA-256', bytes)), byte => byte.toString(16).padStart(2, '0')).join('')
    const release = { board: 'BOARD', version: 'browser-test', image: {
      evMuxVersion: 4, chipFamily: 0x3280, chipRevision: 302,
      offset: 65536, bytes: data.byteLength, partitionSize: 4096,
      sha256: await hash(data), partitionTable: { offset: 32768, bytes: table.byteLength, sha256: await hash(table) },
    } }
    let stubs = 0, writes = 0, resets = 0
    prototype.initialize = async function () { this.chipFamily = 0x3280; this.chipRevision = 302 }
    prototype.getSecurityInfo = async () => ({ flags: 1, flashCryptCnt: 0 })
    prototype.runStub = async () => { stubs++; throw new Error('must not launch') }
    const device = context.DeviceCore.createDevice()
    const request = {
      release, board: 'BOARD', data,
      openLoader: ({ signal }) => context.DeviceCore.openBrowserRom({ port: fakePort(), signal }),
    }
    await assert.rejects(device.firmware.update(request), /Secure boot/)
    assert.equal(stubs, 0, 'ROM security must be checked before the browser launches a stub')
    await assert.rejects(device.firmware.update({ ...request, data: new Uint8Array(4) }), /SHA-256/)
    assert.equal(stubs, 0)
    prototype.getSecurityInfo = async () => ({ flags: 0, flashCryptCnt: 0 })
    const writing = new AbortController()
    prototype.runStub = async () => {
      stubs++
      return {
        IS_STUB: true,
        async flashData() { writes++; writing.abort() },
        async readFlash(offset) { return offset === 32768 ? table : data },
        async readRegister() { return 0 },
      }
    }
    prototype.hardReset = async () => { resets++ }
    const beforeUpdateCloses = disconnectCalls
    const updated = await device.firmware.update({ ...request, signal: writing.signal })
    assert.equal(updated.verified, true)
    assert.equal(updated.needsReset, false)
    assert.equal(stubs, 1); assert.equal(writes, 1); assert.equal(resets, 1)
    assert.equal(disconnectCalls, beforeUpdateCloses + 1, 'write-time abort must preserve complete readback, reset and one cleanup')
    const manifest = { schemaVersion: 1, version: 'browser-test', sourceCommit: 'a'.repeat(40), pipelineId: 1, layout: 'legacy',
      releases: [{ ...release, image: { ...release.image, url: 'https://dl.espressif.com/AE/esp-vision/bin/app.bin' } }],
      website: { schemaVersion: '1.0.0', release: 'browser-test', releases: ['browser-test'], binaryNameTemplate: '{board}-{tag}.bin',
        boards: [{ board: 'BOARD', name: 'Board', slug: 'board', flashable: true }], models: [] }, mcp: null }
    const rawManifest = new TextEncoder().encode(JSON.stringify(manifest)), caches = []
    const snapshot = await context.DeviceCore.loadReleaseManifest({ url: 'https://dl.espressif.com/AE/esp-vision/releases/browser-test/manifest.json', sha256: await hash(rawManifest),
      async fetch(_url, options) {
        caches.push(options.cache)
        if (caches.length === 1) throw new TypeError('Cached response CORS failure from the host realm')
        return new Response(rawManifest)
      } })
    assert.equal(snapshot.version, 'browser-test')
    assert.deepEqual(caches, ['default', 'no-store'], 'real browser bundle must recover a fetch TypeError from host-provided fetch')

  } finally { Object.assign(prototype, originals) }
  // Node import must not eagerly resolve tasmota's browser-only, extensionless ESM.
  const { openBrowserRom } = await import(pathToFileURL(join(root, 'src/browser.js')))
  assert.equal(typeof openBrowserRom, 'function')
  const dependency = JSON.parse(await readFile(join(root, 'node_modules/tasmota-webserial-esptool/package.json'), 'utf8'))
  assert.equal(dependency.version, '9.4.2')
  const stubsLicense = await readFile(join(root, 'node_modules/tasmota-webserial-esptool/src/stubs/README.md'), 'utf8')
  assert.match(stubsLicense, /dual licensed.*Apache License Version 2.0 or the MIT license/s)
  await writeFile(join(temporary, 'browser.js'), code)
  console.log(JSON.stringify({ browserBundle: true, dependency: dependency.version, bytes: code.length, passiveImports: true, p4AndS31Stubs: true, cleanupAndCancellation: true, coreSafetyChecks: true }))
} finally { await rm(temporary, { recursive: true, force: true }) }

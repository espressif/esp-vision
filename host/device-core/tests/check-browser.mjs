// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { mkdtemp, readFile, writeFile, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join, dirname } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import vm from 'node:vm'
import { webcrypto, createHash } from 'node:crypto'
import { build } from 'esbuild'

const root = dirname(dirname(fileURLToPath(import.meta.url)))
const temporary = await mkdtemp(join(tmpdir(), 'esp-vision-browser-'))
try {
  const result = await build({
    stdin: { contents: `export { createDevice } from './src/index.js'; export { openBrowserRom } from './src/browser.js'; export { loadReleaseManifest } from './src/firmware.js'; export { ESPLoader as TestLoader, Transport as TestTransport } from 'esptool-js'; export { browserTransportCompatibility as testTransport } from './src/browser_rom_compat.js';`, resolveDir: root },
    bundle: true, platform: 'browser', format: 'iife', globalName: 'DeviceCore', write: false, metafile: true,
  })
  const code = result.outputFiles[0].text, inputs = Object.keys(result.metafile.inputs)
  assert.ok(inputs.some(path => path.includes('esptool-js/lib/esploader.js')))
  for (const chip of ['esp32p4', 'esp32s31']) assert.ok(inputs.some(path => path.includes(`stub_flasher/${chip}.json`)))
  assert.ok(!inputs.some(path => /tasmota|src\/(node|cli)/.test(path)))
  const context = vm.createContext({ atob, btoa, EventTarget, URL, TextDecoder, TextEncoder, Uint8Array, DataView, ArrayBuffer, AbortController, setTimeout, clearTimeout, console, crypto: webcrypto, structuredClone })
  vm.runInContext(code, context)
  let opens = 0
  const adapter = context.DeviceCore.openBrowserRom({ port: {
    getInfo: () => ({ usbVendorId: 0x10c4, usbProductId: 0xea60 }),
    async open() { opens++; throw new Error('real dependency reached SerialPort.open') }, async close() {},
  } })
  assert.equal(opens, 0)
  await assert.rejects(adapter.initialize(), /real dependency reached SerialPort.open/)
  assert.equal(opens, 1)
  const transport = context.DeviceCore.testTransport(new context.DeviceCore.TestTransport({ getInfo: () => ({}), readable: {}, writable: {} }, false))
  transport.buffer = new Uint8Array([71, 97, 114, 0xc0, 79, 72, 65, 73, 0xc0])
  assert.deepEqual(Array.from(await transport.read(100)), [79, 72, 65, 73])
  transport.buffer = new Uint8Array([0xc0, 0xdb, 0x47, 0xc0])
  await assert.rejects(transport.read(100), /Invalid SLIP escape/)
  let released = 0
  transport.device.writable = { getWriter: () => ({ async write() { throw new Error('USB detached during write') }, releaseLock() { released++ } }) }
  await assert.rejects(transport.write(new Uint8Array([1])), /detached/)
  assert.equal(released, 1, 'failed writes release the real WebSerial writer lock')
  const prototype = context.DeviceCore.TestLoader.prototype
  const keys = ['detectChip', 'getSecurityInfo', 'runStub', 'writeFlash', 'readReg', 'writeReg', 'checkCommand']
  const originals = Object.fromEntries(keys.map(key => [key, prototype[key]]))
  let detects = 0, closes = 0, stubs = 0, writes = 0, resets = 0
  const fakePort = () => ({
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 32 }),
    async open() { this.readable = {}; this.writable = {} },
    async close() { closes++; this.readable = null; this.writable = null },
    async setSignals() { throw new Error('OTG must not require UART control signals') },
  })
  try {
    prototype.detectChip = async function () { detects++; await this.transport.connect(115200); this.chip = { CHIP_NAME: 'ESP32-S31', getChipRevision: async () => 0, readMac: async () => '30:ed:a0:ed:a1:b0' } }
    prototype.getSecurityInfo = async () => ({ flags: 0, flashCryptCnt: 0 })
    const controller = new AbortController(), loader = context.DeviceCore.openBrowserRom({ port: fakePort(), signal: controller.signal, expectedMac: '30EDA0EDA1B0' })
    assert.equal(loader.chipMac, null)
    const first = loader.initialize()
    assert.equal(loader.initialize(), first)
    await first; assert.equal(loader.chipMac, '30eda0eda1b0'); controller.abort()
    assert.equal(closes, 0, 'abort after initialization preserves ongoing flash')
    await loader.disconnect(); await loader.disconnect()
    assert.equal(closes, 1); assert.equal(detects, 1)
    const detect = prototype.detectChip
    prototype.detectChip = async function () { await this.transport.connect(115200); throw new Error('sync failed') }
    await assert.rejects(context.DeviceCore.openBrowserRom({ port: fakePort() }).initialize(), /USB-OTG ROM is not ready/)
    assert.equal(closes, 2)
    prototype.detectChip = detect
    let finishOpen
    const waitingPort = fakePort(), open = waitingPort.open
    waitingPort.open = async function () { await new Promise(resolve => { finishOpen = resolve }); await open.call(this) }
    const cancelled = new AbortController(), late = context.DeviceCore.openBrowserRom({ port: waitingPort, signal: cancelled.signal })
    const starting = late.initialize()
    while (!finishOpen) await new Promise(resolve => setTimeout(resolve, 1))
    cancelled.abort(); await assert.rejects(starting, { name: 'AbortError' })
    finishOpen(); await new Promise(resolve => setTimeout(resolve, 5))
    assert.equal(closes, 3); assert.equal(waitingPort.readable, null)
    prototype.runStub = async function () { this.IS_STUB = true }
    let finishErase
    prototype.checkCommand = () => new Promise(resolve => { finishErase = resolve })
    const eraseCancel = new AbortController(), erasePort = fakePort()
    const eraseLoader = context.DeviceCore.openBrowserRom({ port: erasePort, signal: eraseCancel.signal })
    await eraseLoader.initialize(); const eraseStub = await eraseLoader.runStub()
    const erasing = eraseStub.eraseRegion(0x710000, 4096)
    const failedErase = assert.rejects(erasing, { name: 'AbortError' })
    eraseCancel.abort(); await failedErase
    finishErase(0)
    assert.equal(closes, 4); assert.equal(erasePort.readable, null)
    await assert.rejects(eraseStub.eraseRegion(0x710000, 4096))
    const data = new Uint8Array([0xe9, 1, 2, 3]), table = new Uint8Array([4, 5])
    const hash = async bytes => Array.from(new Uint8Array(await webcrypto.subtle.digest('SHA-256', bytes)), byte => byte.toString(16).padStart(2, '0')).join('')
    const release = { board: 'BOARD', version: 'browser-test', image: {
      evMuxVersion: 4, chipFamily: 0x3231, chipRevision: 0, offset: 65536, bytes: data.byteLength, partitionSize: 4096,
      sha256: await hash(data), partitionTable: { offset: 32768, bytes: table.byteLength, sha256: await hash(table) },
    } }
    const device = context.DeviceCore.createDevice(), request = { release, board: 'BOARD', data,
      openLoader: ({ signal }) => context.DeviceCore.openBrowserRom({ port: fakePort(), signal }),
    }
    prototype.getSecurityInfo = async () => ({ flags: 1, flashCryptCnt: 0 })
    prototype.runStub = async () => { stubs++; throw new Error('must not launch') }
    await assert.rejects(device.firmware.update(request), /Secure boot/)
    assert.equal(stubs, 0)
    await assert.rejects(device.firmware.update({ ...request, data: new Uint8Array(4) }), /SHA-256/)
    assert.equal(stubs, 0)
    prototype.getSecurityInfo = async () => ({ flags: 0, flashCryptCnt: 0 })
    prototype.runStub = async function () { stubs++; this.IS_STUB = true }
    const writing = new AbortController()
    prototype.writeFlash = async function (options) { assert.equal(options.compress, true); assert.equal(options.eraseAll, false); writes++; writing.abort(); options.reportProgress(0, 4, 4) }
    prototype.readReg = async () => 0
    prototype.writeReg = async () => { resets++ }
    prototype.checkCommand = async function (description, _opcode, packet) {
      assert.equal(description, 'read flash')
      const address = new DataView(packet.buffer).getUint32(0, true), bytes = address === 32768 ? table : data
      const digest = createHash('md5').update(bytes).digest()
      const frame = chunk => [0xc0, ...Array.from(chunk).flatMap(byte => byte === 0xc0 ? [0xdb, 0xdc] : byte === 0xdb ? [0xdb, 0xdd] : [byte]), 0xc0]
      this.transport.buffer = new Uint8Array([...frame(bytes), ...frame(digest)])
      return 0
    }
    const wrongPort = fakePort(), wrong = context.DeviceCore.openBrowserRom({ port: wrongPort, expectedMac: '30eda0eda1b1' })
    const beforeStubs = stubs, beforeWrites = writes
    await assert.rejects(wrong.initialize(), { code: 'ROM_IDENTITY_CHANGED' })
    assert.equal(wrong.chipMac, null); assert.equal(wrongPort.readable, null)
    await assert.rejects(async () => wrong.runStub(), { code: 'ROM_CLOSED' })
    assert.equal(stubs, beforeStubs); assert.equal(writes, beforeWrites)
    const updatePort = fakePort(), openUpdate = updatePort.open
    updatePort.open = async function (...args) { await openUpdate.apply(this, args); this.writable = { getWriter: () => ({ async write() {}, releaseLock() {} }) } }
    const beforeCloses = closes
    const updated = await device.firmware.update({ ...request, signal: writing.signal, openLoader: ({ signal }) => context.DeviceCore.openBrowserRom({ port: updatePort, signal, expectedMac: '30eda0eda1b0' }) })
    assert.equal(updated.verified, true); assert.equal(updated.needsReset, false)
    assert.equal(stubs, 1); assert.equal(writes, 1); assert.equal(resets, 4)
    assert.equal(closes, beforeCloses + 1)
    const manifest = { schemaVersion: 1, version: 'browser-test', sourceCommit: 'a'.repeat(40), pipelineId: 1, layout: 'legacy',
      releases: [{ ...release, image: { ...release.image, url: 'https://dl.espressif.com/AE/esp-vision/bin/app.bin' } }],
      website: { schemaVersion: '1.0.0', release: 'browser-test', releases: ['browser-test'], binaryNameTemplate: '{board}-{tag}.bin', boards: [{ board: 'BOARD', name: 'Board', slug: 'board', flashable: true }], models: [] }, mcp: null }
    const rawManifest = new TextEncoder().encode(JSON.stringify(manifest)), caches = []
    const snapshot = await context.DeviceCore.loadReleaseManifest({ url: 'https://dl.espressif.com/AE/esp-vision/releases/browser-test/manifest.json', sha256: await hash(rawManifest),
      async fetch(_url, options) { caches.push(options.cache); if (caches.length === 1) throw new TypeError('Cached CORS failure from host realm'); return new Response(rawManifest) },
    })
    assert.equal(snapshot.version, 'browser-test'); assert.deepEqual(caches, ['default', 'no-store'])
  } finally { Object.assign(prototype, originals) }
  const { openBrowserRom } = await import(pathToFileURL(join(root, 'src/browser.js')))
  assert.equal(typeof openBrowserRom, 'function')
  const dependency = JSON.parse(await readFile(join(root, 'node_modules/esptool-js/package.json'), 'utf8'))
  assert.equal(dependency.version, '0.7.1'); assert.equal(dependency.license, 'Apache-2.0')
  assert.match(await readFile(join(root, 'node_modules/esptool-js/LICENSE'), 'utf8'), /Apache License/)
  await writeFile(join(temporary, 'browser.js'), code)
  console.log(JSON.stringify({ browserBundle: true, dependency: dependency.version, bytes: code.length, passiveImports: true, p4AndS31Stubs: true, cleanupAndCancellation: true, coreSafetyChecks: true, compressedWrite: true, fullReadbackAndMd5: true }))
} finally { await rm(temporary, { recursive: true, force: true }) }

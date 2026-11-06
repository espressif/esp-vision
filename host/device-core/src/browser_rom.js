// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Loaded only by openBrowserRom.initialize(), never by the root/Node entry point.
import { ESPLoader, Transport } from 'esptool-js'
import { browserRomCompatibility, browserTransportCompatibility, readBrowserFlash, romError, romWords } from './browser_rom_compat.js'
import { normalizeExpectedMac } from './rom_identity.js'
import { romMd5Hex } from './rom_md5.js'

export const ROM_UART_BAUD_RATES = Object.freeze([2000000, 1500000, 921600, 460800, 115200])
const families = { 'ESP32-S3': 0x3253, 'ESP32-P4': 0x3280, 'ESP32-S31': 0x3231 }
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms))
const securityAllowed = security => Number.isSafeInteger(security?.flags) && Number.isSafeInteger(security?.flashCryptCnt)
  && (security.flags & 5) === 0 && security.flashCryptCnt.toString(2).replace(/0/g, '').length % 2 === 0

export function browserRomInterface(port, requested = 'auto') {
  if (!['auto', 'uart', 'usb-otg', 'usb-jtag'].includes(requested)) throw new TypeError('ROM interface must be auto, uart, usb-otg or usb-jtag')
  if (requested !== 'auto') return requested
  const { usbVendorId: vid, usbProductId: pid } = port.getInfo?.() ?? {}
  if (vid === 0x303a && pid === 0x1001) return 'usb-jtag'
  if (vid === 0x303a && [2, 9, 18, 32].includes(pid)) return 'usb-otg'
  if ([0x10c4, 0x1a86, 0x0403, 0x067b].includes(vid)) return 'uart'
  // Custom descriptors are ambiguous. Probe without reset, then use the chip's
  // connection registers; callers may explicitly identify an external bridge.
  return 'unknown'
}

export function createBrowserLoader(port, logger = {}, options = {}) {
  return createOfficialBrowserLoader({ ESPLoader, Transport }, port, logger, options)
}

// Dependency injection is internal to the package and permits lifecycle/error
// testing against fake official-loader dependencies without browser globals.
export function createOfficialBrowserLoader({ ESPLoader: Loader, Transport: SerialTransport }, port, logger = {}, {
  interface: requestedInterface = 'auto', baudRate = 'auto', onStatus = () => {}, signal, expectedMac,
} = {}) {
  const expected = normalizeExpectedMac(expectedMac)
  let kind = browserRomInterface(port, requestedInterface)
  if (baudRate !== 'auto' && !ROM_UART_BAUD_RATES.includes(baudRate)) throw new RangeError('Unsupported ROM UART baudRate')
  let loader, transport, identity, initialized = false, active = false, fault, writing = false, disconnected = false
  const status = event => { logger.log?.(`[ROM] ${JSON.stringify(event)}`); onStatus(event) }
  const assertReady = () => {
    if (disconnected) throw romError('ROM_CLOSED', 'Browser ROM adapter is closed')
    if (fault) throw fault
    if (!initialized) throw romError('ROM_NOT_INITIALIZED', 'ROM synchronization and chip qualification are required')
  }
  async function closeTransport() {
    if (transport && (transport.device?.readable || transport.device?.writable)) await transport.disconnect()
  }
  async function detect(mode) {
    transport = browserTransportCompatibility(new SerialTransport(port, false))
    loader = browserRomCompatibility(new Loader({ transport, baudrate: 115200, romBaudrate: 115200, serialOptions: { bufferSize: 256 * 1024 },
      terminal: { clean() {}, write: value => logger.log?.(value), writeLine: value => logger.log?.(value) },
    }), { nativeUsb: kind === 'usb-otg' || kind === 'usb-jtag' })
    // The stub replies once to SYNC; the ROM replies eight times. In 0.7.1 the
    // unconditional eight-reply wait prevents reconnecting to a running stub.
    loader.sync = async function () {
      const request = new Uint8Array(36).fill(0x55); request.set([7, 7, 18, 32])
      let response = await this.command(8, request, 0, true, 500)
      this.syncStubDetected = response[0] === 0
      if (!this.syncStubDetected) for (let i = 0; i < 7; i++) response = await this.readPacket(8, 500)
      return response
    }
    try {
      await loader.detectChip(mode, 3)
      if (disconnected) throw romError('ROM_CLOSED', 'Browser ROM adapter is closed')
      if (!loader.chip || !families[loader.chip.CHIP_NAME]) throw romError('ROM_CHIP_UNSUPPORTED', 'Browser flashing supports ESP32-S3, ESP32-P4 and ESP32-S31')
      const revision = await loader.chip.getChipRevision(loader)
      if (typeof loader.chip.readMac !== 'function') throw romError('ROM_IDENTITY_INVALID', 'The ROM chip does not expose its factory MAC reader')
      const rawMac = await loader.chip.readMac(loader)
      // The pinned official chip API returns six colon-separated hex octets.
      // Keep register layouts and byte ordering owned by that implementation.
      if (typeof rawMac !== 'string' || rawMac.length !== 17 || !/^(?:[0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(rawMac)) throw romError('ROM_IDENTITY_INVALID', 'The ROM did not return a complete factory MAC')
      const mac = rawMac.replaceAll(':', '').toLowerCase()
      const current = { name: loader.chip.CHIP_NAME, family: families[loader.chip.CHIP_NAME], revision, mac }
      if ((expected !== undefined && current.mac !== expected)
          || (identity && (identity.family !== current.family || identity.revision !== current.revision || identity.mac !== current.mac))) {
        throw romError('ROM_IDENTITY_CHANGED', 'ROM identity differs from the selected device or the bound download session')
      }
      if (disconnected) throw romError('ROM_CLOSED', 'Browser ROM adapter is closed')
      identity = current
      if (kind === 'unknown') {
        if (await loader.usesUsbOtg()) kind = 'usb-otg'
        // Without descriptor/register evidence, retain the conservative mode.
      }
      initialized = true
      status({ phase: 'rom-connected', interface: kind, baudRate: transport.baudrate })
    } catch (error) {
      initialized = active = false
      await closeTransport().catch(() => {})
      if (error.code === 'ROM_IDENTITY_CHANGED' || error.code === 'ROM_IDENTITY_INVALID') {
        fault = error
        disconnected = true
        throw error
      }
      const lost = /device.*lost|disconnected|setSignals|networkerror/i.test(`${error.name} ${error.message}`)
      if (lost) throw romError('ROM_PORT_LOST', 'Download port detached or re-enumerated; select its current ROM port again', error)
      if (kind === 'usb-otg') throw romError('ROM_MANUAL_DOWNLOAD', 'USB-OTG ROM is not ready. Hold BOOT, press RESET, release BOOT, then select the ROM port again', error)
      throw error
    }
  }
  async function qualifyAndStartStub() {
    const security = await loader.getSecurityInfo(false)
    if (!securityAllowed(security)) throw romError('ROM_SECURITY', 'Secure boot, secure download or encrypted devices require their signed flashing workflow')
    if (typeof loader.chip.postConnect === 'function') await loader.chip.postConnect(loader)
    await loader.runStub()
    if (!loader.IS_STUB) throw romError('ROM_STUB_REQUIRED', 'This device has no supported browser flash stub')
    active = true
  }
  async function recover() {
    initialized = active = false
    await closeTransport()
    // A reopen alone is never recovery. Reset/sync at 115200, identify the chip,
    // recheck security and start a validated stub before another candidate.
    await detect('default_reset')
    await qualifyAndStartStub()
  }
  async function preflight() {
    const register = loader.chip.EFUSE_BLOCK1_ADDR ?? loader.CHIP_DETECT_MAGIC_REG_ADDR
    const expected = await loader.readReg(register, 1000)
    for (let count = 0; count < 3; count++) if (await loader.readReg(register, 1000) !== expected) throw romError('ROM_BAUD_PROBE', 'Register response changed during UART speed verification')
    // Read-only bulk traffic checks SLIP, acknowledgements and a device digest.
    await readBrowserFlash(loader, 0, 4096)
  }
  async function chooseBaud() {
    if (kind !== 'uart') {
      status({ phase: 'baud-ready', interface: kind, baudRate: null })
      return
    }
    const candidates = baudRate === 'auto' ? ROM_UART_BAUD_RATES : [baudRate]
    for (let index = 0; index < candidates.length; index++) {
      const candidate = candidates[index]
      try {
        if (index) await recover()
        status({ phase: 'baud-testing', interface: kind, baudRate: candidate })
        if (candidate !== 115200) {
          // Use the official checked command and transport retune primitives.
          // The upstream changeBaud silently falls back to 115200; Core needs
          // each candidate's actual result to obey its ordered selection policy.
          await loader.checkCommand('change baud rate', loader.ESP_CHANGE_BAUDRATE, romWords(candidate, transport.baudrate), 0, 0, 1000)
          await sleep(50)
          await transport.changeBaudrate(candidate, loader.serialOptions)
          loader.baudrate = candidate
          transport.flushInput()
        }
        await preflight()
        status({ phase: 'baud-ready', interface: kind, baudRate: candidate })
        return
      } catch (error) {
        status({ phase: 'baud-failed', interface: kind, baudRate: candidate, code: error.code ?? 'ROM_BAUD_FAILED', message: error.message })
        if (error.code === 'ROM_IDENTITY_CHANGED' || error.code === 'ROM_IDENTITY_INVALID' || error.code === 'ROM_SECURITY' || error.code === 'ROM_PORT_LOST') throw error
        if (index === candidates.length - 1) throw romError('ROM_BAUD_FAILED', 'No requested UART download speed passed verification', error)
      }
    }
  }
  async function operation(callback) {
    assertReady()
    if (!active || !loader.IS_STUB) throw romError('ROM_STUB_REQUIRED', 'A validated flasher stub is required')
    try { return await callback() }
    catch (error) {
      // Neither failed reads nor compressed writes can be resumed on a merely
      // reopened port. Invalidate the session; the caller must start afresh.
      fault = error
      initialized = active = false
      await closeTransport().catch(() => {})
      throw error
    }
  }
  const stub = {
    get IS_STUB() { return active && !!loader?.IS_STUB },
    flashId: () => operation(async () => {
      // RDID requests 24 bits, while readFlashId exposes the entire signed
      // SPI_W0 register. Upper unused bits may be set on real S31 hardware.
      const raw = await loader.readFlashId()
      if (!Number.isInteger(raw) || raw < -0x80000000 || raw > 0xffffffff) throw romError('ROM_FLASH_ID', 'Invalid SPI flash ID register response')
      const id = raw & 0xffffff
      status({ phase: 'flash-id', id })
      return id
    }),
    readRegister: address => operation(() => loader.readReg(address)),
    writeRegister: (address, value, mask = 0xffffffff) => operation(() => loader.writeReg(address, value, mask)),
    readFlash: (address, size, progress, options) => operation(() => readBrowserFlash(loader, address, size, progress, options)),
    eraseRegion(address, size) {
      return operation(async () => {
        if (!Number.isSafeInteger(address) || address < 0 || address % 4096 !== 0
            || !Number.isSafeInteger(size) || size <= 0 || size > 0xffffffff || size % 4096 !== 0
            || address + size > 0x100000000) throw new RangeError('ROM erase requires a positive, sector-aligned 32-bit address range')
        if (loader.ESP_ERASE_REGION !== 0xd1 || typeof loader.timeoutPerMb !== 'function'
            || !Number.isFinite(loader.ERASE_REGION_TIMEOUT_PER_MB) || loader.ERASE_REGION_TIMEOUT_PER_MB <= 0) throw romError('ROM_ERASE_UNSUPPORTED', 'Use the qualified esptool-js region erase primitives')
        const cancelled = () => signal.reason instanceof Error ? signal.reason : Object.assign(romError('CANCELLED', 'Flash region erase cancelled'), { name: 'AbortError' })
        if (signal?.aborted) throw cancelled()
        let abort
        const aborted = new Promise((_, reject) => {
          abort = () => { reject(cancelled()); void adapter.disconnect().catch(() => {}) }
        })
        signal?.addEventListener('abort', abort, { once: true })
        try {
          if (signal?.aborted) throw cancelled()
          status({ phase: 'erasing-region', address, size })
          // 0.7.1 exposes the region command and checked request primitives,
          // but no eraseRegion convenience method. Retain its framing, status
          // parser and size-scaled timeout rather than implementing transport.
          const erasing = loader.checkCommand('erase region', loader.ESP_ERASE_REGION, romWords(address, size), 0, 0,
            loader.timeoutPerMb(loader.ERASE_REGION_TIMEOUT_PER_MB, size))
          const result = await (signal ? Promise.race([erasing, aborted]) : erasing)
          assertReady()
          if (signal?.aborted) throw cancelled()
          if (!transport.device?.readable || !transport.device?.writable) throw romError('ROM_PORT_LOST', 'Serial device disconnected during Flash region erase')
          if (!active || !loader.IS_STUB) throw romError('ROM_STUB_REQUIRED', 'Flash region erase lost its validated stub')
          if (result !== 0) throw romError('ROM_ERASE_STATUS', `Flash region erase command failed: ${result}`)
        } finally { signal?.removeEventListener('abort', abort) }
      })
    },
    flashData(data, progress, address, _compress) {
      return operation(async () => {
        if (writing) throw romError('ROM_WRITE_REUSE', 'Start a fresh download session before another firmware write')
        writing = true
        let receipt
        status({ phase: 'writing', interface: kind, baudRate: kind === 'uart' ? transport.baudrate : null, compress: true })
        await loader.writeFlash({ fileArray: [{ data: new Uint8Array(data), address }], flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep', eraseAll: false, compress: true,
          calculateMD5Hash(bytes) {
            const digest = romMd5Hex(bytes)
            receipt = { algorithm: 'md5', offset: address, bytes: bytes.length, digest }
            return digest
          },
          reportProgress: (_file, written, total) => progress?.(written, total),
        })
        if (!receipt) throw romError('ROM_WRITE_VERIFY_MISSING', 'Official esptool did not verify the written image')
        // Upstream pads short inputs to four bytes; legacy unaligned callers
        // retain exact-length readback instead of receiving a mismatched receipt.
        return receipt.bytes === data.byteLength ? receipt : undefined
      })
    },
  }
  const adapter = {
    get chipName() { return identity?.name ?? null },
    get chipFamily() { return identity?.family ?? null },
    get chipRevision() { return identity?.revision ?? null },
    get chipMac() { return identity?.mac ?? null },
    get interface() { return kind },
    get baudRate() { return kind === 'uart' ? transport?.baudrate ?? 115200 : null },
    async initialize() { if (disconnected) throw romError('ROM_CLOSED', 'Browser ROM adapter is closed'); await detect(kind === 'uart' || kind === 'usb-jtag' ? 'default_reset' : 'no_reset') },
    async getSecurityInfo() { assertReady(); return loader.getSecurityInfo() },
    async runStub() {
      assertReady()
      try { await qualifyAndStartStub(); await chooseBaud(); return stub }
      catch (error) { fault = error; initialized = active = false; await closeTransport().catch(() => {}); throw error }
    },
    async hardReset() {
      assertReady()
      if (kind === 'usb-otg' && identity.family === 0x3231) {
        // S31's LP_WDT register layout (ESP-IDF soc/rtc_wdt_reg.h). The generic
        // watchdog sequence also appears in Apache-2.0 esptool-js P4 support;
        // 0.7.1's S31 inherits an unimplemented C5 watchdog method.
        const base = 0x20801000
        await loader.writeReg(base + 0x18, 0x50d83aa1)
        await loader.writeReg(base + 4, 2000)
        await loader.writeReg(base, 0xd0000102)
        await loader.writeReg(base + 0x18, 0)
        await sleep(500)
        return
      }
      if (kind !== 'uart' && typeof loader.chip.watchdogReset === 'function' && identity.family === 0x3280) return loader.chip.watchdogReset(loader)
      if (kind === 'usb-otg' || kind === 'unknown') throw romError('ROM_RESET_UNSUPPORTED', 'Firmware verified; press RESET to start the application on this interface')
      // esptool-js 0.7.1 HardReset only deasserts RTS. Pulse it explicitly so
      // bridge-connected devices actually restart after verified flashing.
      await transport.setDTR(false)
      await transport.setRTS(true)
      await sleep(100)
      await transport.setRTS(false)
    },
    async disconnect() { disconnected = true; initialized = active = false; await closeTransport() },
  }
  return adapter
}

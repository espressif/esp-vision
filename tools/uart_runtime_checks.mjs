// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

// Browser/Node acceptance through the same public Device Core application API.
export async function checkUartRuntime(device, { role, expectedBaud, report = () => {}, saveFrame = async () => {} } = {}) {
    const assert = (condition, message) => { if (!condition) throw new Error(message) }
    const signal = AbortSignal.timeout(180000)
    const nonce = crypto.randomUUID().replaceAll('-', '').slice(0, 12)
    const marker = `EVUART_${nonce}`
    let ownedPath, frameCount = 0, frameBytes = 0, firstFrame
    const diagnostics = []
    const offDiagnostic = device.on('diagnostic', event => {
        if (event.event?.includes('baud')) diagnostics.push(event)
    })
    const offPreview = device.on('preview', frame => {
        const data = frame.data
        assert(data[0] === 255 && data[1] === 216 && data[data.length - 2] === 255 && data[data.length - 1] === 217, 'Invalid JPEG boundaries')
        firstFrame ??= data.slice()
        frameCount++
        frameBytes += data.length
    })
    try {
        const status = await device.connect({ role, timeout: 65000, signal })
        assert(status.identity?.board === 'ESP32_S31_KORVO', 'Unexpected board; stop acceptance')
        const currentBaud = status.capabilities?.uartBaud?.current
        if (expectedBaud !== undefined && role === 'uart') assert(currentBaud === expectedBaud, `Expected ${expectedBaud} UART baud, got ${currentBaud}`)
        report({ check: 'connect', ok: true, role, version: status.identity.firmwareVersion, uartBaud: currentBaud, diagnostics })
        const execute = async source => {
            const result = await device.python.execute({ source, timeout: 45000, maxOutputChars: 4096, signal })
            assert(result.ok, `Python execution failed: ${result.output}`)
            return result.output
        }
        assert((await execute(`print('${marker}')`)).split(/\r?\n/).includes(marker), 'Missing execution marker')
        report({ check: 'python', ok: true })
        const storage = await device.inspect({ fields: ['storage'], signal })
        assert(storage.storage?.flash?.mounted && storage.storage.flash.writable, `Flash filesystem unavailable: ${storage.storage?.flash?.reason}`)
        const path = `/evuart_${nonce}.bin`
        const entries = await device.files.list({ path: '/', signal })
        assert(!entries.some(entry => [path, `${path}.tmp`].includes(entry.path)), 'Acceptance path already exists')
        const data = Uint8Array.from({ length: 16384 }, (_, i) => (i * 73 + Math.floor(i / 256) * 19 + 29) & 255)
        ownedPath = path
        await device.files.write({ path, data, overwrite: false, signal })
        const readback = await device.files.read({ path, maxBytes: data.length, timeout: 60000, signal })
        assert(readback.length === data.length && data.every((value, i) => readback[i] === value), 'Binary file readback mismatch')
        report({ check: 'file-write-read', ok: true, bytes: data.length })
        const camera = `import sensor, time\nsensor.reset()\nsensor.set_pixformat(sensor.RGB565)\nsensor.set_framesize(sensor.QVGA)\nsensor.skip_frames(time=500)\nfor _ev_i in range(12):\n sensor.snapshot().flush()\n time.sleep_ms(150)\nprint('${marker}_CAMERA')`
        assert((await execute(camera)).split(/\r?\n/).includes(`${marker}_CAMERA`), 'Missing camera completion marker')
        assert(frameCount >= 3, `Only ${frameCount} preview frames received`)
        await saveFrame(firstFrame)
        report({ check: 'preview', ok: true, frames: frameCount, bytes: frameBytes })
        await device.files.remove({ path: ownedPath, signal })
        ownedPath = null
        await device.control({ action: 'reset', mode: 'soft', timeout: 20000, signal })
        assert((await execute(`print('${marker}_RESET')`)).split(/\r?\n/).includes(`${marker}_RESET`), 'REPL failed after soft reset')
        report({ check: 'soft-reset', ok: true })
        return { ok: true, role, uartBaud: currentBaud, frameCount, frameBytes }
    } finally {
        if (ownedPath) {
            try { await device.files.remove({ path: ownedPath, signal: AbortSignal.timeout(5000) }) }
            catch (error) { report({ check: 'cleanup', ok: false, message: error.message, path: ownedPath }) }
        }
        offPreview()
        offDiagnostic()
        await device.disconnect()
    }
}

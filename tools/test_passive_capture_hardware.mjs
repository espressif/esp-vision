// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { createHash, randomUUID } from 'node:crypto'
import { mkdir, writeFile } from 'node:fs/promises'
import { resolve } from 'node:path'
import { setTimeout as sleep } from 'node:timers/promises'
import { createDevice } from '../host/device-core/src/index.js'
import { serialTransport } from '../host/device-core/src/node.js'

// Reset lifecycle checks require boot.py/main.py that do not start the camera
// or display. Prepare and restore that fixture outside this tool: VM ready is
// published before startup files run, and their new frames are not stale data.
// Keep raw responses even when validation fails, so a codec or lifecycle issue
// can be distinguished from an incorrect test precondition.
const args = new Map()
for (let i = 2; i < process.argv.length; i++) {
    const key = process.argv[i]
    if (key === '--legacy') { args.set(key, true); continue }
    if (!['--serial', '--role', '--board', '--output', '--python'].includes(key) || !process.argv[i + 1]) {
        throw new Error('Use --serial PATH --role uart|usb --board BOARD --output DIRECTORY [--python PYTHON] [--legacy]')
    }
    args.set(key, process.argv[++i])
}
assert(args.has('--serial') && args.has('--board') && args.has('--output'), 'Select a port, board and output directory explicitly')
assert(['uart', 'usb'].includes(args.get('--role')), 'Select the physical interface role')
const output = resolve(args.get('--output'))
await mkdir(output, { recursive: true })
const signal = AbortSignal.timeout(240000)
const records = []
const report = record => { records.push(record); console.log(JSON.stringify(record)) }
const nonce = randomUUID().replaceAll('-', '').slice(0, 12)
const variable = `__capture_test_${nonce}`
const mount = `/capture_${nonce}`
let device, mounted = false, running = false
let phase = 'connect', captureIndex = 0
const open = async (probeRepl = true) => {
    device = createDevice({ transport: serialTransport({ serial: args.get('--serial'), python: args.get('--python') ?? 'python3' }) })
    const status = await device.connect({ role: args.get('--role'), probeRepl, signal })
    assert.equal(status.identity.board, args.get('--board'), 'Unexpected board; stop acceptance')
    report({ check: 'connect', ok: true, role: args.get('--role'), identity: status.identity,
        uartBaud: status.capabilities.uartBaud?.current })
    return status
}
const execute = async source => {
    const result = await device.python.execute({ source, timeout: 15000, maxOutputChars: 12000, signal })
    assert(result.ok, result.output)
    assert(!/Traceback \(most recent call last\)/.test(result.output), result.output)
    return result.output
}
// Keep the acceptance independent of persistent storage, including boards with
// an unmounted user volume. Every call stays below the friendly-REPL limit.
const executeRam = async source => {
    const chunks = Array.from(source).join('').match(/[\s\S]{1,80}/g) ?? ['']
    for (let i = 0; i < chunks.length; i++) await execute(`${variable}${i ? '+=' : '='}${JSON.stringify(chunks[i])}`)
    try { return await execute(`exec(${variable})`) }
    finally { await execute(`del ${variable}`) }
}
const expectCode = async (operation, code, stage) => {
    phase = stage
    const codes = Array.isArray(code) ? code : [code]
    let actual
    await assert.rejects(operation, error => { actual = error.code; return codes.includes(actual) })
    report({ check: actual, stage, ok: true })
}
const capture = async stage => {
    phase = stage
    const image = await device.capture({ source: 'camera', quality: 60, timeout: 15000, signal })
    assert(image.data instanceof Uint8Array, 'Capture did not return binary bytes')
    const file = `${args.get('--role')}-${String(++captureIndex).padStart(3, '0')}-${stage}.jpg`
    await writeFile(resolve(output, file), image.data)
    report({ check: 'capture-received', stage, file, bytes: image.data.length, metadata: image.metadata,
        sha256: createHash('sha256').update(image.data).digest('hex'),
        head: Array.from(image.data.subarray(0, 16)), tail: Array.from(image.data.subarray(-32)) })
    return image
}
const jpegHash = image => {
    assert.equal(image.mimeType, 'image/jpeg')
    assert(image.data.length > 4 && image.data[0] === 255 && image.data[1] === 216
        && image.data.at(-2) === 255 && image.data.at(-1) === 217, 'Invalid JPEG')
    return createHash('sha256').update(image.data).digest('hex')
}
const readJson = async expression => {
    const text = await execute(`import json; print('CAPTURE_RESULT:'+json.dumps(${expression}))`)
    const line = text.split(/\r?\n/).find(line => line.startsWith('CAPTURE_RESULT:'))
    assert(line, text)
    return JSON.parse(line.slice('CAPTURE_RESULT:'.length))
}

try {
    const status = await open()
    if (args.has('--legacy')) {
        assert(!status.capabilities.features.includes('debug.capture_frame.observed.v1'))
        await expectCode(() => capture('legacy-camera'), 'UNSUPPORTED_OPERATION', 'legacy-camera-refused')
        report({ check: 'legacy-camera-refused', ok: true })
    } else {
        assert(status.capabilities.features.includes('debug.capture_frame.observed.v1'))
        phase = 'startup-soft-reset'
        await device.control({ action: 'reset', mode: 'soft', signal })
        await expectCode(() => capture('startup-camera'), 'CAMERA_FRAME_NOT_READY', 'startup-camera-empty')
        await expectCode(() => device.capture({ source: 'screen', signal }), ['DISPLAY_NOT_READY', 'NOT_SUPPORTED'], 'startup-screen-empty')
        for (const external of [false, true]) {
            const name = external ? 'external' : 'default'
            phase = `${name}-configure`
            await execute('import sensor,binascii; sensor.reset(); sensor.set_pixformat(sensor.RGB565); sensor.set_framesize(sensor.QQVGA)')
            await expectCode(() => capture(`${name}-before-snapshot`), 'CAMERA_FRAME_NOT_READY', `${name}-before-snapshot`)
            phase = `${name}-publish`
            await execute(external
                ? '_capture_buf=bytearray(160*120*2); _capture_img=sensor.snapshot(_capture_buf)'
                : '_capture_img=sensor.snapshot(); _capture_buf=_capture_img.bytearray()')
            await execute('_capture_buf[:]=bytes([165])*len(_capture_buf); _capture_crc=binascii.crc32(_capture_buf)')
            const before = await readJson('_capture_crc')
            const first = await capture(`${name}-first`), hash = jpegHash(first)
            await writeFile(resolve(output, `${args.get('--role')}-${external ? 'external' : 'default'}.jpg`), first.data)
            for (let i = 0; i < 3; i++) assert.equal(jpegHash(await capture(`${name}-repeat-${i + 1}`)), hash, 'Passive observation changed without an application capture')
            phase = `${name}-unchanged-settings`
            await execute('sensor.set_framesize(sensor.QQVGA); sensor.set_pixformat(sensor.RGB565); sensor.set_hmirror(sensor.get_hmirror()); sensor.set_vflip(sensor.get_vflip())')
            assert.equal(jpegHash(await capture(`${name}-same-settings`)), hash, 'Unchanged settings invalidated the published frame')
            phase = `${name}-verify-buffer`
            const after = await readJson('binascii.crc32(_capture_buf)')
            assert.equal(after, before, 'Passive capture overwrote the application image')
            report({ check: external ? 'external-buffer-held' : 'default-buffer-held', ok: true, before, after, jpegSha256: hash })
            phase = `${name}-change-mirror`
            await execute('sensor.set_hmirror(not sensor.get_hmirror())')
            await expectCode(() => capture(`${name}-after-mirror`), 'CAMERA_FRAME_NOT_READY', `${name}-mirror-invalidated`)
            phase = `${name}-change-size`
            await execute('sensor.set_framesize(sensor.QVGA)')
            await expectCode(() => capture(`${name}-after-size`), 'CAMERA_FRAME_NOT_READY', `${name}-size-invalidated`)
        }

        phase = 'worker-camera-rejection'
        await executeRam(`import _thread,time\n_capture_thread=[]\ndef _capture_worker():\n try:\n  sensor.shutdown()\n  _capture_thread.append('unexpected-success')\n except OSError:\n  _capture_thread.append('rejected')\n_thread.start_new_thread(_capture_worker,())\nfor _capture_wait in range(100):\n if _capture_thread:\n  break\n time.sleep_ms(10)\n`)
        assert.deepEqual(await readJson('_capture_thread'), ['rejected'])
        report({ check: 'worker-camera-rejected', ok: true })

        phase = 'running-app-prepare'
        // This tiny read-only Python VFS holds only our test program in RAM.
        // It neither mounts nor formats a physical block device or user volume.
        const readyMarker = `CAPTURE_READY_${nonce}`
        const program = `import sensor,time,binascii,__main__\nsensor.reset()\nsensor.set_pixformat(sensor.RGB565)\nsensor.set_framesize(sensor.QQVGA)\n_capture_held=sensor.snapshot()\n_capture_raw=_capture_held.bytearray()\n_capture_raw[:]=bytes([90])*len(_capture_raw)\n_capture_before=binascii.crc32(_capture_raw)\n_capture_changes=0\nprint('${readyMarker}')\n_capture_start=time.ticks_ms()\nwhile time.ticks_diff(time.ticks_ms(),_capture_start)<10000:\n _capture_changes+=int(binascii.crc32(_capture_raw)!=_capture_before)\n time.sleep_ms(50)\n__main__._capture_result={'before':_capture_before,'after':binascii.crc32(_capture_raw),'changes':_capture_changes}\n`
        await executeRam(`import vfs,io\nclass _CaptureVfs:\n def mount(self,readonly,mkfs): pass\n def umount(self): pass\n def stat(self,path):\n  if path in ('','/'):\n   return (16384,0,0,0,0,0,0,0,0,0)\n  if path in ('hold.py','/hold.py'):\n   return (32768,0,0,0,0,0,${program.length},0,0,0)\n  raise OSError(2)\n def open(self,path,mode='r',*args,**kwargs):\n  if path not in ('hold.py','/hold.py'):\n   raise OSError(2)\n  if mode not in ('r','rb'):\n   raise OSError(30)\n  src=${JSON.stringify(program)}\n  return io.BytesIO(src.encode()) if 'b' in mode else io.StringIO(src)\n_capture_vfs=_CaptureVfs()\nvfs.mount(_capture_vfs,${JSON.stringify(mount)})\n`)
        mounted = true
        let seen = '', resolveReady, readyTimer
        const ready = new Promise((resolve, reject) => {
            resolveReady = resolve
            readyTimer = setTimeout(() => reject(new Error('Application did not publish a ready marker')), 15000)
        })
        ready.catch(() => {})
        const off = device.on('python.output', text => { seen += text; if (seen.includes(readyMarker)) resolveReady() })
        try {
            const started = await device.python.execute({ path: `${mount}/hold.py`, completion: 'start', signal })
            started.done.catch(() => {})
            running = true
            await ready
        } finally { clearTimeout(readyTimer); off() }
        const readyAt = Date.now()
        await device.disconnect()
        phase = 'running-app-reconnect'
        await open(false)
        const first = await capture('running-app-first'), hash = jpegHash(first)
        for (let i = 0; i < 3; i++) assert.equal(jpegHash(await capture(`running-app-repeat-${i + 1}`)), hash)
        await sleep(Math.max(0, 11000 - (Date.now() - readyAt)), undefined, { signal })
        running = false
        const result = await readJson("__import__('__main__')._capture_result")
        assert.equal(result.changes, 0)
        assert.equal(result.before, result.after)
        report({ check: 'running-application-across-session-release', ok: true, ...result, jpegSha256: hash })
        await execute(`vfs.umount(${JSON.stringify(mount)}); del _capture_vfs`)
        mounted = false
        phase = 'final-soft-reset'
        await device.control({ action: 'reset', mode: 'soft', signal })
        await expectCode(() => capture('final-camera'), 'CAMERA_FRAME_NOT_READY', 'final-camera-empty')
        report({ check: 'complete', ok: true })
    }
} catch (error) {
    report({ check: 'failed', stage: phase, ok: false, code: error.code, message: error.message, stack: error.stack })
    process.exitCode = 1
} finally {
    if (device) {
        if (running) await device.control({ action: 'stop', timeout: 5000 }).catch(() => {})
        if (mounted) await execute(`vfs.umount(${JSON.stringify(mount)})`).catch(() => {})
        await device.disconnect().catch(error => report({ check: 'disconnect', ok: false, message: error.message }))
    }
    await writeFile(resolve(output, `${args.get('--role')}-${args.has('--legacy') ? 'legacy' : 'capture'}.json`), JSON.stringify(records, null, 2) + '\n')
}

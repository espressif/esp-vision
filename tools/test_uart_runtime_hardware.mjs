// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { writeFile, mkdir } from 'node:fs/promises'
import { resolve } from 'node:path'
import { createDevice } from '../host/device-core/src/index.js'
import { serialTransport } from '../host/device-core/src/node.js'
import { checkUartRuntime } from './uart_runtime_checks.mjs'

const args = new Map()
for (let i = 2; i < process.argv.length; i += 2) {
    const key = process.argv[i], value = process.argv[i + 1]
    if (!['--serial', '--role', '--python', '--expected-baud', '--output'].includes(key) || !value) throw new Error('Expected --serial PATH --role uart|usb --output DIRECTORY [--python PYTHON] [--expected-baud N]')
    args.set(key, value)
}
if (!args.has('--serial') || !['uart', 'usb'].includes(args.get('--role')) || !args.has('--output')) throw new Error('Explicit serial port, role and output directory required')
const output = resolve(args.get('--output'))
await mkdir(output, { recursive: true })
const records = []
const role = args.get('--role')
const report = event => { records.push(event); console.log(JSON.stringify(event)) }
const device = createDevice({ transport: serialTransport({ serial: args.get('--serial'), python: args.get('--python') ?? 'python3' }) })
try {
    const result = await checkUartRuntime(device, {
        role, expectedBaud: args.has('--expected-baud') ? Number(args.get('--expected-baud')) : undefined, report,
        saveFrame: bytes => writeFile(resolve(output, `${role}-preview.jpg`), bytes),
    })
    report({ check: 'complete', ...result })
} catch (error) {
    report({ check: 'failed', ok: false, code: error.code, message: error.message, stack: error.stack })
    process.exitCode = 1
} finally {
    await writeFile(resolve(output, `${role}-runtime.json`), JSON.stringify(records, null, 2) + '\n')
}

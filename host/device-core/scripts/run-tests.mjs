// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { readdir } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import process from 'node:process'

const root = fileURLToPath(new URL('../', import.meta.url))
const tests = (await readdir(new URL('../tests/', import.meta.url)))
  .filter(name => /^test-.*\.mjs$/.test(name)).sort()
if (!tests.length) throw new Error('No Device Core unit tests found; the package is incomplete')
const result = spawnSync(process.execPath, ['--test', ...process.argv.slice(2), ...tests.map(name => `tests/${name}`)], {
  cwd: root, stdio: 'inherit',
})
if (result.error) throw result.error
process.exitCode = result.status ?? 1

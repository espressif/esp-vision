// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import process from 'node:process'

// npm.cmd cannot be passed to execFile/spawn without a Windows shell. Invoke
// npm's JavaScript entry point instead, preserving paths and arguments verbatim.
export function npmCommand(args, env = process.env) {
  if (env.npm_execpath) return [process.execPath, [env.npm_execpath, ...args]]
  if (process.platform === 'win32') {
    const bundled = join(dirname(process.execPath), 'node_modules', 'npm', 'bin', 'npm-cli.js')
    if (!existsSync(bundled)) throw new Error('Run this check through npm run so npm_execpath identifies npm')
    return [process.execPath, [bundled, ...args]]
  }
  return ['npm', args]
}

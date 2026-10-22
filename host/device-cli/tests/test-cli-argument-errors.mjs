// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const entry = fileURLToPath(new URL('../src/cli.js', import.meta.url))

test('argument failures retain the requested command and exit before opening a device', () => {
  const cases = [
    [['python', '--path', '/app.py', '--completion', 'start', '--timeout', '100'], 'python', /--completion wait/],
    [['control', '--action', 'stop', '--mode', 'soft'], 'control', /--mode applies only/],
    [['control', '--action', 'reset'], 'control', /reset requires --mode/],
    [['files', 'read', '/progress.json'], 'files read', /--output/],
    [['python', '--unknown-option'], 'python', /Unknown option/],
    [['python', '--file'], 'python', /argument (?:missing|is ambiguous)/],
    [['--json', '--port', '/nonexistent', 'python', '--completion', 'start', '--source', 'pass'], 'python', /device --path/],
  ]
  for (const [args, command, message] of cases) {
    const result = spawnSync(process.execPath, [entry, ...args, '--port', '/nonexistent', '--json'], { encoding: 'utf8' })
    assert.equal(result.status, 2, result.stderr)
    const lines = result.stdout.trim().split('\n')
    assert.equal(lines.length, 1)
    const envelope = JSON.parse(lines[0])
    assert.equal(envelope.ok, false)
    assert.equal(envelope.command, command, args.join(' '))
    assert.equal(envelope.error.code, 'ARGUMENT')
    assert.match(envelope.error.message, message)
    assert.ok(!result.stderr.includes('"state":"connecting"'))
  }
})

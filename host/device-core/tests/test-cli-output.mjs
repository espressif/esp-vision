// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import stringWidth from 'string-width'
import { formatResult } from '../src/cli.js'

test('wide tables align columns by terminal cells with CJK, combining characters and emoji', () => {
  const devices = ['COM3', '/dev/开发板0', '/dev/e\u0301-👩‍💻'].map((address, index) => ({ number: index + 1, interface: 'usb', address, status: 'unprobed', identity: { board: 'BOARD', firmwareVersion: 'v1' } }))
  const text = formatResult({ devices }, { columns: 1000 })
  assert.ok(!text.includes('\t') && !text.includes('\\t'))
  const lines = text.split('\n')
  assert.equal(lines.length, 4)
  const column = (line, word) => stringWidth(line.slice(0, line.indexOf(word)))
  assert.ok(lines.slice(1).every(line => column(line, 'unprobed') === column(lines[0], 'Status')))
  assert.ok(lines.slice(1).every(line => column(line, 'BOARD') === column(lines[0], 'Board')))
})

test('narrow terminals wrap full identifiers without losing Unicode graphemes or data', () => {
  const address = '/dev/serial/by-id/usb-' + '开发板👩‍💻e\u0301'.repeat(12)
  const text = formatResult({ devices: [{ number: 1, interface: 'usj', address, status: 'unprobed', usb: { serialNumber: 'SERIAL-123' } }] }, { columns: 60 })
  assert.ok(text.split('\n').every(line => stringWidth(line) <= 60))
  const value = text.split('\n').slice(text.split('\n').findIndex(line => line.startsWith('Address'))).join('\n').split('\nStatus')[0]
  assert.equal(value.replace(/^Address\s*: /, '').replace(/\n +/g, ''), address)
  assert.ok(text.includes('USB serial : SERIAL-123'))
})

test('empty discovery is explicit and non-inventory results keep their JSON format', () => {
  assert.equal(formatResult({ devices: [] }), 'No devices found.')
  assert.equal(formatResult({ ok: true }), JSON.stringify({ ok: true }, null, 2))
})

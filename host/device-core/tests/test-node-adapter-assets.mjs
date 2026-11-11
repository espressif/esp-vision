// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import * as api from '../src/node.js'
import { readFile } from 'node:fs/promises'

test('Node API no longer exports interpreter configuration or Python assets', async () => {
  assert.equal('defaultPython' in api, false)
  assert.equal('nodeAdapterAssets' in api, false)
  for (const file of ['node_serial.py', 'node_rom.py']) await assert.rejects(readFile(new URL('../src/' + file, import.meta.url)), { code: 'ENOENT' })
})

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import { test } from 'node:test'
import { nodeAdapterAssets } from '../src/node.js'

test('public Node asset URLs identify the exact passive SDK adapters', async () => {
  const assets = nodeAdapterAssets()
  assert.ok(Object.isFrozen(assets))
  assert.deepEqual(Object.keys(assets).sort(), ['rom', 'serial'])
  assert.match(await readFile(assets.serial, 'utf8'), /serial/)
  assert.match(await readFile(assets.rom, 'utf8'), /esptool/)
  assets.serial.pathname = '/changed-by-consumer'
  assert.ok(nodeAdapterAssets().serial.pathname.endsWith('/node_serial.py'))
})

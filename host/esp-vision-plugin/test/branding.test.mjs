// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { readFile } from 'node:fs/promises'
import { test } from 'node:test'
import { validatePackage } from '../src/installer.mjs'

const root = new URL('../', import.meta.url)
const digest = '9d9aab9c27f0cf9465174467698d5ffecd8b344cfd1d460f01409c6f43611e11'

test('official logo bytes and client metadata survive the validated installation payload', async () => {
  const logo = await readFile(new URL('assets/esp-vision-logo.png', root))
  assert.equal(createHash('sha256').update(logo).digest('hex'), digest)
  const plugin = JSON.parse(await readFile(new URL('plugin.json', root), 'utf8'))
  const claude = JSON.parse(await readFile(new URL('.claude-plugin/plugin.json', root), 'utf8'))
  assert.equal(plugin.extensions['com.openai'].interface.logo, './assets/esp-vision-logo.png')
  assert.equal(plugin.extensions['com.openai'].interface.composerIcon, './assets/esp-vision-logo.png')
  assert.equal(claude.icon, './assets/esp-vision-logo.png')
  const { files } = await validatePackage()
  assert.ok(files.some(file => file.path === 'assets/esp-vision-logo.png'))
  assert.ok(files.some(file => file.path === 'assets/PROVENANCE.md'))
})

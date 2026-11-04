// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { copyFile, cp, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import process from 'node:process'
import { fileURLToPath } from 'node:url'
import { test } from 'node:test'
import { npmCommand } from '../scripts/npm-command.mjs'

const root = fileURLToPath(new URL('../', import.meta.url))

test('npm execution preserves Unicode, spaces and shell metacharacters as literal arguments', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'core npm 中文 '))
  t.after(() => rm(directory, { recursive: true, force: true }))
  const cli = join(directory, 'npm entry.cjs')
  await writeFile(cli, 'process.stdout.write(JSON.stringify(process.argv.slice(2))); process.exitCode = 7\n')
  const args = ['pack', '中文 path', '& echo forbidden', '$(forbidden)', '%PATH%']
  const env = { ...process.env, npm_execpath: cli }
  const result = spawnSync(...npmCommand(args, env), { env, encoding: 'utf8' })
  assert.equal(result.status, 7, result.stderr)
  assert.deepEqual(JSON.parse(result.stdout), args)
})

test('schema check accepts checkout CRLF but rejects a real artifact change', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'core schema '))
  t.after(() => rm(directory, { recursive: true, force: true }))
  await mkdir(join(directory, 'scripts'))
  await mkdir(join(directory, 'schemas'))
  await cp(join(root, 'src'), join(directory, 'src'), { recursive: true })
  await copyFile(join(root, 'package.json'), join(directory, 'package.json'))
  const script = join(directory, 'scripts/generate-action-schema.mjs')
  await copyFile(join(root, 'scripts/generate-action-schema.mjs'), script)
  const schema = join(directory, 'schemas/device-actions-v1.json')
  const original = (await readFile(join(root, 'schemas/device-actions-v1.json'), 'utf8')).replaceAll('\r\n', '\n')
  await writeFile(schema, original.replaceAll('\n', '\r\n'))
  const current = spawnSync(process.execPath, [script, '--check'], { encoding: 'utf8' })
  assert.equal(current.status, 0, current.stderr)
  const changed = JSON.parse(original)
  changed.deviceCoreVersion = '0.0.0'
  await writeFile(schema, JSON.stringify(changed, null, 2).replaceAll('\n', '\r\n') + '\r\n')
  const stale = spawnSync(process.execPath, [script, '--check'], { encoding: 'utf8' })
  assert.notEqual(stale.status, 0)
  assert.match(stale.stderr, /Device Action schema artifact is stale/)
})

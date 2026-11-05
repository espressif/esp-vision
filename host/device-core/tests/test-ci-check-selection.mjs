// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { existsSync, readFileSync } from 'node:fs'
import test from 'node:test'
import { DEVICE_CHECKS } from './device-checks.mjs'
import { parseArgs } from './hardware-check.mjs'

const ciConfig = new URL('../../../.gitlab/ci/target_test.yml', import.meta.url)
test('CI keeps its qualified hardware suite explicit while manual ISP checks remain selectable', {
  skip: !existsSync(ciConfig) && 'Repository CI configuration is not included in the npm package',
}, () => {
  const yaml = readFileSync(ciConfig, 'utf8')
  const commands = yaml.replace(/\\\r?\n\s*/g, ' ').split(/\r?\n/)
    .map(line => line.trim()).filter(line => line.startsWith('python3 tools/ci/flash_and_test.py '))
  assert.equal(commands.length, 1)
  const selections = [...commands[0].matchAll(/(?:^|\s)--checks\s+(\S+)/g)]
  assert.equal(selections.length, 1)
  assert.match(commands[0], /(?:^|\s)--require-all(?:\s|$)/)
  const expected = ['identity', 'memory', 'camera', 'framesizes', 'pixformats', 'orientation',
    'ipa_stats', 'snapshot_rate', 'preview', 'image_ops', 'display', 'display_stream',
    'camera_display_lifecycle', 'image_copy_to_fb', 'image_transform_argument',
    'sensor_snapshot_buffer', 'memory_policy', 'espdl_inference']
  const selected = parseArgs(['--serial', 'policy-test', '--checks', selections[0][1], '--require-all'])
  assert.deepEqual(selected.checks, expected)
  assert.equal(selected.requireAll, true)
  assert.deepEqual(parseArgs(['--serial', 'policy-test', '--checks', 'all']).checks, [...DEVICE_CHECKS])
  const manual = ['isp_ccm_frames', 'isp_lsc_display']
  assert.deepEqual(parseArgs(['--serial', 'policy-test', '--checks', manual.join(',')]).checks, manual)
  for (const name of manual) assert.ok(DEVICE_CHECKS.includes(name))
})

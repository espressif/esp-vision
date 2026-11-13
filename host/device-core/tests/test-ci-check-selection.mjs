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
  assert.equal(commands.length, 3)
  const expected = ['identity', 'discovery', 'communication', 'reconnect', 'modules', 'ulab', 'memory', 'camera', 'framesizes', 'pixformats', 'orientation',
    'ipa_stats', 'snapshot_rate', 'preview', 'image_ops', 'display', 'display_stream',
    'camera_display_lifecycle', 'image_copy_to_fb', 'image_transform_argument',
    'sensor_snapshot_buffer', 'memory_policy', 'espdl_inference']
  const visionUnsupported = ['ipa_stats', 'display', 'display_stream', 'camera_display_lifecycle']
  const expectedByJob = [expected, expected.filter(name => !visionUnsupported.includes(name)),
    expected.filter(name => !['ipa_stats', 'espdl_inference'].includes(name))]
  for (const [index, command] of commands.entries()) {
    const selections = [...command.matchAll(/(?:^|\s)--checks\s+(\S+)/g)]
    assert.equal(selections.length, 1)
    assert.match(command, /(?:^|\s)--require-all(?:\s|$)/)
    const screenOutput = /(?:^|\s)--screen-output(?:\s|$)/.test(command)
    assert.equal(screenOutput, index !== 1)
    const role = command.match(/(?:^|\s)--role\s+(\S+)/)?.[1] ?? 'usj'
    if (index === 2) {
      assert.equal(role, 'uart')
      assert.match(command, /--port "\$ESPPORT"/)
    }
    const selected = parseArgs(['--serial', 'policy-test', '--role', role, '--checks', selections[0][1], '--require-all',
      ...(screenOutput ? ['--screen-output', 'screen.jpg'] : [])])
    assert.deepEqual(selected.checks, expectedByJob[index])
    assert.equal(selected.requireAll, true)
  }
  assert.deepEqual(parseArgs(['--serial', 'policy-test', '--checks', 'all']).checks, [...DEVICE_CHECKS])
  const manual = ['isp_ccm_frames', 'isp_lsc_display']
  assert.deepEqual(parseArgs(['--serial', 'policy-test', '--checks', manual.join(',')]).checks, manual)
  for (const name of manual) assert.ok(DEVICE_CHECKS.includes(name))
})

// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Pack } from '../dist/pack.js';

test('API aliases remain available and link to the canonical documentation', () => {
  const pack = new Pack({
    meta: { canonicalUrlTemplate: 'https://example.invalid/{path}#{anchor}' },
    modules: [{ name: 'display', source: 'esp-vision', targets: ['esp32p4'], symbols: [
      { kind: 'class', name: 'ESP32Display', signatures: ['ESP32Display()'], targets: ['esp32p4'] },
      { kind: 'class', name: 'Display', aliasOf: 'display.ESP32Display', signatures: ['Display()'], targets: ['esp32p4'] },
    ] }],
  });
  const result = pack.getApi('display.Display', 'p4', 'esp-vision');
  assert.equal(result.type, 'symbol');
  assert.equal(result.matches[0].id, 'display.Display');
  assert.equal(result.matches[0].aliasOf, 'display.ESP32Display');
  assert.equal(result.matches[0].available, true);
  assert.equal(result.matches[0].canonicalUrl, 'https://example.invalid/api-reference/display.html#display.ESP32Display');
});

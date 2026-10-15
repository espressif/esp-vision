#!/usr/bin/env node
/*
 * SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */

import assert from 'node:assert/strict'
import { mkdir, readFile, writeFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import process from 'node:process'
import {
  DEVICE_ACTION_DESCRIPTORS,
  DEVICE_ACTION_SCHEMA_VERSION,
} from '../src/device_actions.js'

const coreRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const corePackage = JSON.parse(await readFile(resolve(coreRoot, 'package.json'), 'utf8'))
const outputPath = resolve(coreRoot, `schemas/device-actions-v${DEVICE_ACTION_SCHEMA_VERSION}.json`)
const document = {
  $schema: 'https://json-schema.org/draft/2020-12/schema',
  title: 'ESP Vision Device Actions',
  deviceCoreVersion: corePackage.version,
  schemaVersion: DEVICE_ACTION_SCHEMA_VERSION,
  actions: DEVICE_ACTION_DESCRIPTORS,
}
const serialized = `${JSON.stringify(document, null, 2)}\n`

if (process.argv.includes('--check')) {
  assert.equal(await readFile(outputPath, 'utf8'), serialized, 'Device Action schema artifact is stale; regenerate it')
  console.log(`Device Action Schema ${DEVICE_ACTION_SCHEMA_VERSION} is current`)
} else {
  await mkdir(dirname(outputPath), { recursive: true })
  await writeFile(outputPath, serialized)
  console.log(`Generated ${outputPath}`)
}

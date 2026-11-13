// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { readFileSync } from 'node:fs'
import { pathToFileURL } from 'node:url'

// Consume the existing CLI's successful inspection; never connect or guess.
export function knowledgeVersion(envelope) {
  const version = envelope?.result?.identity?.firmwareVersion
  if (envelope?.ok !== true || envelope.command !== 'inspect' ||
      typeof version !== 'string' || !/^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$/.test(version) ||
      ['unknown', 'latest', 'dev'].includes(version.toLowerCase())) {
    throw new Error('FIRMWARE_VERSION_REQUIRED: inspect the selected device or select a target version; if unavailable, offer an official firmware update before using latest Knowledge with the user\'s explicit decline')
  }
  return { firmwareVersion: version }
}

// Input is the decoded list_firmware_versions result, not a guessed release.
export function fallbackKnowledgeVersion(catalog, { updateDeclined = false } = {}) {
  if (updateDeclined !== true) throw new Error('FIRMWARE_UPDATE_CHOICE_REQUIRED: offer an official firmware update and wait for the user to explicitly decline')
  if (!Array.isArray(catalog?.snapshots) || !Array.isArray(catalog.firmwareVersions)) {
    throw new Error('KNOWLEDGE_CATALOG_REQUIRED: read list_firmware_versions before selecting latest Knowledge')
  }
  // Release order is maintained in the catalog, not inferred across tag formats.
  const version = catalog.latestFirmwareVersion
  if (typeof version !== 'string' || version === 'latest' || !catalog.firmwareVersions.includes(version)) {
    throw new Error('KNOWLEDGE_VERSION_UNAVAILABLE: latest official firmware is absent from the catalog')
  }
  const candidates = catalog.snapshots.filter(snapshot => snapshot?.active === true && snapshot.firmwareVersion === version)
  if (candidates.length !== 1 || typeof candidates[0].snapshotVersion !== 'string' ||
      !/^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$/.test(candidates[0].snapshotVersion)) {
    throw new Error('KNOWLEDGE_CATALOG_INVALID: latest firmware must have exactly one active snapshot')
  }
  const selected = candidates[0]
  return { firmwareVersion: selected.firmwareVersion, snapshotVersion: selected.snapshotVersion }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const args = process.argv.slice(2)
    if (args.length && (args.length !== 1 || args[0] !== '--declined-firmware-update')) throw new Error('Unsupported arguments')
    const input = JSON.parse(readFileSync(0, 'utf8'))
    const selection = args.length ? fallbackKnowledgeVersion(input, { updateDeclined: true }) : knowledgeVersion(input)
    if (args.length) process.stderr.write(`KNOWLEDGE_VERSION_MISMATCH: using Knowledge for ${selection.firmwareVersion} after the user declined an official firmware update; device compatibility is unverified\n`)
    process.stdout.write(JSON.stringify(selection) + '\n')
  }
  catch (error) { process.stderr.write(error.message + '\n'); process.exitCode = 1 }
}

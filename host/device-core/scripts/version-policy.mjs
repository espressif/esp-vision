// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'

const stablePattern = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/
const minorPattern = /^~(0|[1-9]\d*)\.(0|[1-9]\d*)\.0$/

export function stableVersion(value) {
  const match = stablePattern.exec(value ?? '')
  assert.ok(match, `Expected an exact stable major.minor.patch version: ${value}`)
  const numbers = match.slice(1).map(Number)
  assert.ok(numbers.every(Number.isSafeInteger), 'Version components must be safe integers')
  return numbers
}

export function minorRange(value) {
  const match = minorPattern.exec(value ?? '')
  assert.ok(match, `Expected a minor-locked range such as ~0.5.0: ${value}`)
  const numbers = match.slice(1).map(Number)
  assert.ok(numbers.every(Number.isSafeInteger), 'Version components must be safe integers')
  return numbers
}

export function inMinorRange(range, version) {
  const [major, minor] = minorRange(range)
  const [candidateMajor, candidateMinor] = stableVersion(version)
  return candidateMajor === major && candidateMinor === minor
}

// Published older CLI releases may declare an exact Core version; newer
// releases must declare a minor range, enforced at the CLI publisher.
export function declaredCoreSupports(declaration, version) {
  if (typeof declaration !== 'string') return false
  if (stablePattern.test(declaration)) return declaration === version
  return inMinorRange(declaration, version)
}

// The CLI bundles Core. A compatible range describes allowed inputs; the
// resolved version identifies the actual bytes in a published CLI archive.
export function cliBundlesCore(manifest, version) {
  const declaration = manifest.devDependencies?.['@esp-vision/device-core']
  if (!declaredCoreSupports(declaration, version)) return false
  const resolved = manifest.espVision?.resolved?.deviceCore
  if (resolved !== undefined) return resolved === version
  // Older CLI packages used an exact build dependency as their only record.
  return stablePattern.test(declaration ?? '') && declaration === version
}

export function highestStableVersion(versions) {
  let best
  for (const candidate of versions) {
    if (!stablePattern.test(candidate)) continue
    const parts = stableVersion(candidate)
    if (!best || parts.some((value, index) => value !== best.parts[index] &&
      parts.slice(0, index).every((prior, i) => prior === best.parts[i]) && value > best.parts[index])) {
      best = { version: candidate, parts }
    }
  }
  return best?.version
}

export function validateVersionStep(previous, next, change) {
  const [major, minor, patch] = stableVersion(previous)
  const expected = {
    patch: `${major}.${minor}.${patch + 1}`,
    minor: `${major}.${minor + 1}.0`,
    major: `${major + 1}.0.0`,
  }
  assert.ok(Object.hasOwn(expected, change), 'Release type must be patch, minor or major')
  assert.equal(next, expected[change], `A ${change} release after ${previous} must be ${expected[change]}`)
}

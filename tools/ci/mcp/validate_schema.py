#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
#
# Validate an MCP API knowledge pack (docs/gen_mcp.py output) against
# tools/ci/mcp/schema.json, plus a few cross-field invariants the JSON Schema
# cannot express. Used by the CI check gate so a malformed pack fails early.
#
#     python tools/ci/mcp/validate_schema.py mcp-pack.json

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCHEMA_PATH = HERE / 'schema.json'
ESP8266_ONLY_SYMBOLS = {
    'esp.sleep_type',
    'esp.deepsleep',
    'esp.flash_id',
    'esp.set_native_code_location',
}


def _semantic_checks(pack: dict) -> list:
    """Invariants beyond the JSON Schema."""
    errors = []
    targets = set(pack.get('meta', {}).get('targets', []))
    if not targets:
        errors.append('meta.targets is empty')

    cap_targets = {c['target'] for c in pack.get('capabilities', [])}
    missing = targets - cap_targets
    if missing:
        errors.append('capabilities missing target(s): {}'.format(sorted(missing)))

    seen = set()
    actual_counts = {
        'espVisionModules': 0,
        'espVisionSymbols': 0,
        'micropythonModules': 0,
        'micropythonSymbols': 0,
        'docs': len(pack.get('docs', [])),
        'examples': len(pack.get('examples', [])),
    }
    for m in pack.get('modules', []):
        source = m.get('source')
        if source == 'esp-vision':
            actual_counts['espVisionModules'] += 1
        elif source == 'micropython':
            actual_counts['micropythonModules'] += 1
        for s in m.get('symbols', []):
            sid = '{}.{}'.format(m['name'], s['name'])
            if source == 'esp-vision':
                actual_counts['espVisionSymbols'] += 1
            elif source == 'micropython':
                actual_counts['micropythonSymbols'] += 1
            if sid in seen:
                errors.append('duplicate symbol id: {}'.format(sid))
            seen.add(sid)
            if sid in ESP8266_ONLY_SYMBOLS:
                errors.append('ESP8266-only symbol present in ESP32 pack: {}'.format(sid))
            for sig in s.get('signatures', []):
                stripped = sig.strip()
                if stripped.startswith(':'):
                    errors.append('symbol {} has RST option as signature: {}'.format(sid, sig))
                if (s.get('kind') in ('function', 'method', 'class')
                        and '=' in stripped and '(' not in stripped):
                    errors.append('symbol {} has orphan signature continuation: {}'.format(
                        sid, sig))
            for t in s.get('targets', []):
                if t not in targets:
                    errors.append('symbol {} has unknown target {!r}'.format(sid, t))

    counts = pack.get('meta', {}).get('counts', {})
    for key, actual in actual_counts.items():
        if key in counts and counts[key] != actual:
            errors.append('meta.counts.{} is {}, expected {}'.format(
                key, counts[key], actual))

    if not any(m['source'] == 'esp-vision' for m in pack.get('modules', [])):
        errors.append('no esp-vision modules found (stub extraction broken?)')
    return errors


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: validate_schema.py <pack.json>', file=sys.stderr)
        return 2
    pack = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
    schema = json.loads(SCHEMA_PATH.read_text(encoding='utf-8'))

    errors = []
    try:
        import jsonschema  # type: ignore
        validator = jsonschema.Draft7Validator(schema)
        for e in sorted(validator.iter_errors(pack), key=lambda x: list(x.path)):
            loc = '/'.join(str(p) for p in e.path) or '<root>'
            errors.append('{}: {}'.format(loc, e.message))
    except ImportError:
        print('[validate_schema] jsonschema not installed; running semantic '
              'checks only (install jsonschema in CI for full validation).')

    errors.extend(_semantic_checks(pack))

    if errors:
        print('FAIL: {} problem(s):'.format(len(errors)), file=sys.stderr)
        for e in errors[:50]:
            print('  - {}'.format(e), file=sys.stderr)
        return 1

    c = pack.get('meta', {}).get('counts', {})
    print('OK: pack valid (schemaVersion {}, version {}, counts {})'.format(
        pack.get('schemaVersion'), pack.get('meta', {}).get('version'), c))
    return 0


if __name__ == '__main__':
    sys.exit(main())

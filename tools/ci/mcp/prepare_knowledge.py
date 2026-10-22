#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Produce one checked npm artifact shared by publication and image creation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-version')
    parser.add_argument('--expected-source-commit')
    args = parser.parse_args()
    pack = args.pack.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    data = pack.read_bytes()
    metadata = json.loads(data)['meta']
    if args.expected_source_commit and metadata['sourceCommit'] != args.expected_source_commit:
        parser.error('Knowledge source commit differs from the requested image/release')
    check = [sys.executable, str(ROOT / 'tools/ci/mcp/validate_schema.py'), str(pack), '--require-micropython']
    if args.expected_version:
        check += ['--expected-version', args.expected_version]
    subprocess.run(check, check=True, cwd=ROOT)
    package = ROOT / 'host/knowledge-mcp'
    env = {**os.environ, 'MCP_PACK_PATH': str(pack)}
    subprocess.run(['npm', 'run', 'build:release'], cwd=package, env=env, check=True)
    # Build runs once. All downstream checks, publication and Docker consume
    # the exact npm archive generated here, without rebuilding its sources.
    result = subprocess.run(['npm', 'pack', '--json', '--ignore-scripts', '--pack-destination', str(output)],
                            cwd=package, env=env, check=True, text=True, capture_output=True)
    packed = json.loads(result.stdout)[0]
    tarball = output / 'knowledge-mcp.tgz'
    shutil.copyfile(output / packed['filename'], tarball)
    subprocess.run(['node', 'scripts/check-package.mjs', '--tarball', str(tarball)], cwd=package, env=env, check=True)
    archive = tarball.read_bytes()
    receipt = {'schemaVersion': 1, 'name': packed['name'], 'version': packed['version'],
               'file': tarball.name, 'sha256': hashlib.sha256(archive).hexdigest(),
               'integrity': packed['integrity'], 'bytes': len(archive),
               'packVersion': metadata['version'], 'sourceCommit': metadata['sourceCommit'],
               'packSha256': hashlib.sha256(data).hexdigest()}
    (output / 'knowledge-npm.json').write_text(json.dumps(receipt, indent=2) + '\n')
    shutil.copyfile(pack, output / 'mcp-pack.json')
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()

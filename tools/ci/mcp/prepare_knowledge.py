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

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-version')
    parser.add_argument('--expected-source-commit', required=True)
    args = parser.parse_args()
    catalog = args.catalog.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    data = catalog.read_bytes()
    from catalog import validate_catalog
    validate_catalog(data)
    package = ROOT / 'host/knowledge-mcp'
    version = json.loads((package / 'package.json').read_bytes())['version']
    if args.expected_version and version != args.expected_version:
        parser.error('Knowledge npm version differs from release pipeline version')
    env = {**os.environ, 'MCP_CATALOG_PATH': str(catalog), 'MCP_RUNTIME_SOURCE_COMMIT': args.expected_source_commit or ''}
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
               'sourceCommit': args.expected_source_commit, 'catalogSha256': hashlib.sha256(data).hexdigest()}
    (output / 'knowledge-npm.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if catalog != output / 'mcp-catalog.json':
        shutil.copyfile(catalog, output / 'mcp-catalog.json')
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()

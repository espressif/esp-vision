#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Run the unified local MCP/SDK/plugin/website acceptance with retained logs."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--website', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pack', type=Path, help='Additionally test and validate a complete generated knowledge pack')
    parser.add_argument('--install', action='store_true')
    parser.add_argument('--port', help='Additionally run packaged CLI acceptance on an attached P4 board')
    parser.add_argument('--python', default='python3', help='Serial Python interpreter for target acceptance')
    parser.add_argument('--native-codex', type=Path, help='Actual Codex executable for isolated native installation checks')
    parser.add_argument('--native-claude', type=Path, help='Actual Claude executable for isolated native installation checks')
    parser.add_argument('--previous-plugin-tarball', type=Path, help='Optional previous public plugin archive; native tests otherwise use a synthetic upgrade fixture')
    args = parser.parse_args()
    native = [args.native_codex, args.native_claude]
    if (any(native) and not all(native)) or (args.previous_plugin_tarball and not all(native)):
        parser.error('Native acceptance requires both client executables')
    website, output = args.website.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    temporary = output / 'tmp'
    temporary.mkdir(exist_ok=True)
    env = {**os.environ, 'TMPDIR': str(temporary), 'TMP': str(temporary), 'TEMP': str(temporary),
           'npm_config_cache': str(output / 'npm-cache')}
    if args.pack:
        env['MCP_TEST_PACK_PATH'] = str(args.pack.resolve())
    major = subprocess.check_output(['node', '-p', 'process.versions.node.split(".")[0]'], env=env, text=True).strip()
    if major != '24':
        parser.error('Select Node 24 on PATH to match the Knowledge MCP container')
    env['ESP_VISION_NPM_TARBALL_DIR'] = str(output / 'artifacts')
    pack = args.pack.resolve() if args.pack else output / 'knowledge-pack.json'
    commands = []
    if args.install:
        commands.extend([
            ('install-host-artifacts', ROOT, ['node', 'tools/ci/mcp/install_host.mjs', '--output', str(output / 'core-artifact')]),
            ('knowledge-install', ROOT, ['npm', '--prefix', 'host/knowledge-mcp', 'ci', '--ignore-scripts', '--no-audit', '--no-fund']),
            ('website-install', website, ['pnpm', 'install', '--frozen-lockfile', '--ignore-scripts']),
        ])
    if not args.pack:
        commit = subprocess.check_output(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, text=True).strip()
        commands.append(('generate-knowledge', ROOT, [sys.executable, 'docs/gen_mcp.py', 'acceptance-' + commit, '--out', str(pack)]))
    commands.extend([
        ('device-core-tests', ROOT, ['npm', '--prefix', 'host/device-core', 'test']),
        ('device-core-schema', ROOT, ['npm', '--prefix', 'host/device-core', 'run', 'check:schema']),
        ('device-core-browser', ROOT, ['npm', '--prefix', 'host/device-core', 'run', 'check:browser']),
        ('device-core-tarball', ROOT, ['npm', '--prefix', 'host/device-core', 'run', 'check:package']),
        ('device-cli-build', ROOT, ['npm', '--prefix', 'host/device-cli', 'run', 'build']),
        ('device-cli-tests', ROOT, ['npm', '--prefix', 'host/device-cli', 'test']),
        ('device-cli-npm-dry-run', ROOT, ['node', 'tools/ci/mcp/release_npm.mjs', '--dry-run', '--package', 'host/device-cli']),
        ('plugin-build', ROOT, ['npm', '--prefix', 'host/esp-vision-plugin', 'run', 'build']),
        ('plugin-installer-tests', ROOT, ['npm', '--prefix', 'host/esp-vision-plugin', 'test']),
        ('plugin-npm-dry-run', ROOT, ['node', 'tools/ci/mcp/release_npm.mjs', '--dry-run', '--package', 'host/esp-vision-plugin']),
        ('plugin-archive', ROOT, ['node', 'host/esp-vision-plugin/scripts/archive.mjs', '--output-dir', str(output / 'artifacts')]),
        ('knowledge-build', ROOT, ['npm', '--prefix', 'host/knowledge-mcp', 'run', 'build']),
        ('knowledge-tests', ROOT, ['npm', '--prefix', 'host/knowledge-mcp', 'test']),
        ('knowledge-npm-artifact', ROOT, [sys.executable, 'tools/ci/mcp/prepare_knowledge.py', '--pack', str(pack), '--output', str(output / 'knowledge-artifact')]),
        ('knowledge-npm-dry-run', ROOT, ['node', 'tools/ci/mcp/release_npm.mjs', '--dry-run', '--package', 'host/knowledge-mcp', '--tarball', str(output / 'knowledge-artifact/knowledge-mcp.tgz')]),
        ('knowledge-npm-transports-statistics', ROOT, ['node', 'tools/ci/mcp/smoke_knowledge.mjs', '--tarball', str(output / 'knowledge-artifact/knowledge-mcp.tgz')]),
        ('producer-contract', ROOT, [sys.executable, '-m', 'unittest', 'discover', '-s', 'tools/ci/mcp', '-p', 'test_*.py']),
        ('npm-publication-policy', ROOT, ['node', '--test', 'tools/ci/mcp/test_release_npm.mjs']),
        ('main-host-regressions', ROOT, [sys.executable, '-m', 'unittest', 'discover', '-s', 'tools/tests', '-p', 'test_*.py']),
        ('website-types', website, ['pnpm', 'exec', 'tsc', '--noEmit']),
        ('website-lint', website, ['pnpm', 'run', 'lint']),
        ('website-firmware', website, ['pnpm', 'run', 'test:firmware']),
        ('website-wifi', website, ['pnpm', 'run', 'test:wifi']),
        ('website-release', website, ['pnpm', 'run', 'test:release-sync']),
        ('website-deployment', website, [sys.executable, '-m', 'unittest', 'discover', '-s', 'scripts', '-p', 'test_release_*.py']),
        ('website-stats-report', website, [sys.executable, '-m', 'unittest', 'discover', '-s', 'scripts', '-p', 'test_mcp_usage_report.py']),
        ('website-build', website, ['pnpm', 'run', 'build']),
        ('main-doc-format', ROOT, [sys.executable, 'tools/ci/check_docs_format.py']),
        ('main-doc-translations', ROOT, [sys.executable, 'tools/ci/check_docs_lang_sync.py']),
        ('cross-repository-contract', ROOT, [sys.executable, 'tools/ci/mcp/check_consumer.py', '--website', str(website), '--artifacts', str(output / 'knowledge-artifact'), '--output', str(output / 'cross-repository')]),
        ('main-diff-check', ROOT, ['git', 'diff', '--check']),
        ('website-diff-check', website, ['git', 'diff', '--check']),
    ])
    if args.port:
        cli_version = json.loads((ROOT / 'host/device-cli/package.json').read_text())['version']
        consumer = output / 'target-consumer'
        commands.extend([
            ('target-cli-install', ROOT, ['npm', 'install', '--prefix', str(consumer), '--ignore-scripts',
                                         '--no-audit', '--no-fund', str(output / f'artifacts/esp-vision-device-cli-{cli_version}.tgz')]),
            ('target-cli-acceptance', ROOT, ['node', 'tools/ci/mcp/target_cli_acceptance.mjs',
                                           '--cli', str(consumer / 'node_modules/@esp-vision/device-cli/dist/device-cli.mjs'),
                                           '--port', args.port, '--python', args.python, '--output', str(output / 'target')]),
        ])
    if all(native):
        version = json.loads((ROOT / 'host/esp-vision-plugin/package.json').read_text())['version']
        commands.append(('native-plugin-acceptance', ROOT, ['node', 'tools/ci/mcp/native_plugin_acceptance.mjs',
                         '--tarball', str(output / f'artifacts/esp-vision-plugin-{version}.tgz'),
                         '--codex', str(args.native_codex.resolve()), '--claude', str(args.native_claude.resolve()),
                         '--output', str(output / 'native'),
                         *(['--previous-tarball', str(args.previous_plugin_tarball.resolve())] if args.previous_plugin_tarball else [])]))
    results = []
    for name, cwd, command in commands:
        path = output / (name.replace('/', '-') + '.log')
        started = time.monotonic()
        with path.open('w') as log:
            result = subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
        results.append({'check': name, 'exitCode': result.returncode,
                        'seconds': round(time.monotonic() - started, 3), 'log': str(path)})
        (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print('{} {}'.format('PASS' if result.returncode == 0 else 'FAIL', name), flush=True)
        if result.returncode:
            print(path.read_text()[-8000:])
            return result.returncode
    print('Unified acceptance passed; container/runner validation is a separate CI gate.')
    return 0


if __name__ == '__main__':
    sys.exit(main())

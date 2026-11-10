# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Reject known private/work-in-progress paths in the Git index, even if ignored."""

from pathlib import PurePosixPath
import subprocess
import sys


PRIVATE_DIRECTORIES = {
    'specs', '.specs', '.codex', '.claude', '.cursor', '.agents',
    '.vscode', '.idea', '.esp-vision', '.worktrees',
}
ROOT_WORK_DIRECTORIES = {'plans', 'scratch', 'investigations', 'handoffs'}
PRIVATE_FILES = {'.mcp.json', '.npmrc', '.pypirc', '.netrc'}
ENV_TEMPLATES = {'.env.example', '.env.template'}
# These exact files are shipped product source. Do not allow the whole directory.
PUBLIC_PLUGIN_FILES = {
    'host/esp-vision-plugin/.agents/plugins/marketplace.json',
    'host/esp-vision-plugin/.mcp.json',
}


def prohibited(path):
    if path in PUBLIC_PLUGIN_FILES:
        return False
    parts = PurePosixPath(path).parts
    if any(part in PRIVATE_DIRECTORIES for part in parts):
        return True
    if parts[0] in ROOT_WORK_DIRECTORIES:
        return True
    name = parts[-1]
    return name in PRIVATE_FILES or (
        (name == '.env' or name.startswith('.env.')) and name not in ENV_TEMPLATES
    )


def check_index():
    # No --exclude-standard: force-added ignored files must still fail. The index
    # excludes staged deletions and includes files missing from the working tree.
    paths = subprocess.check_output(['git', 'ls-files', '--cached', '-z'])
    return sorted({
        path.decode('utf-8', errors='surrogateescape')
        for path in paths.split(b'\0')
        if path and prohibited(path.decode('utf-8', errors='surrogateescape'))
    })


def main():
    try:
        violations = check_index()
    except (OSError, subprocess.CalledProcessError) as error:
        print('Unable to inspect the Git index: {}'.format(error), file=sys.stderr)
        return 2
    if violations:
        print('Private configuration or working artifacts are staged/tracked:', file=sys.stderr)
        for path in violations:
            print('  {!r}'.format(path), file=sys.stderr)
        print(
            'Keep working material outside the checkout (ESP_VISION_WORK_DIR). '
            'See AGENTS.md: Repository content boundary.', file=sys.stderr,
        )
        return 1
    print('Repository hygiene: no prohibited paths in the Git index.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

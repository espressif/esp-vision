#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Warn when public-facing source changes are not accompanied by docs changes."""

import fnmatch
import os
import subprocess


SOURCE_PATTERNS = (
    'example/**',
    'modules/**',
    'components/imlib/**',
    'components/zxing/**',
    'boards/**/*.cmake',
    'boards/**/*.h',
    'boards/**/manifest.py',
    'boards/**/board_inisetup.py',
    'micropython.cmake',
    'overlay/micropython/**/*.c',
    'overlay/micropython/**/*.h',
    'overlay/micropython/**/*.cmake',
)


def git(*args):
    return subprocess.check_output(('git',) + args, text=True).strip()


def matches(path, patterns):
    return any(fnmatch.fnmatch(path, pattern) for pattern in patterns)


def diff_base():
    candidates = (
        os.getenv('CI_MERGE_REQUEST_DIFF_BASE_SHA'),
        os.getenv('CI_COMMIT_BEFORE_SHA'),
    )
    for candidate in candidates:
        if candidate and candidate != '0' * 40:
            return candidate

    target_branch = os.getenv('CI_MERGE_REQUEST_TARGET_BRANCH_NAME')
    if target_branch:
        remote_ref = 'origin/{}'.format(target_branch)
        try:
            git('rev-parse', '--verify', remote_ref)
            return remote_ref
        except subprocess.CalledProcessError:
            pass
    return 'HEAD^'


def main():
    head = (
        os.getenv('CI_MERGE_REQUEST_SOURCE_BRANCH_SHA')
        or os.getenv('CI_COMMIT_SHA')
        or 'HEAD'
    )
    changed = git('diff', '--name-only', '{}...{}'.format(diff_base(), head)).splitlines()
    source_changes = sorted(path for path in changed if matches(path, SOURCE_PATTERNS))
    docs_changed = any(
        path.startswith('docs/') or path.startswith('stubs/')
        for path in changed
    )

    if not source_changes or docs_changed:
        print('Documentation impact check passed.')
        return 0

    print('WARNING: Public-facing source changed without a docs/ or stubs/ update.')
    print('Review whether the following files require documentation changes:')
    for path in source_changes:
        print('  - {}'.format(path))
    print('This check is advisory and does not block the merge request.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

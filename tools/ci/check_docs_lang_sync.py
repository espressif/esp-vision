#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Check that the English and Chinese documentation trees have matching pages."""

from pathlib import Path


def rst_files(root):
    return {
        path.relative_to(root)
        for path in root.rglob('*.rst')
        if '_generated' not in path.parts
    }


def main():
    docs_dir = Path(__file__).resolve().parents[2] / 'docs'
    en_files = rst_files(docs_dir / 'en')
    zh_files = rst_files(docs_dir / 'zh_CN')

    only_en = sorted(en_files - zh_files)
    only_zh = sorted(zh_files - en_files)
    if not only_en and not only_zh:
        print('English and Chinese documentation page sets are synchronized.')
        return 0

    if only_en:
        print('Pages missing from docs/zh_CN:')
        for path in only_en:
            print('  {}'.format(path))
    if only_zh:
        print('Pages missing from docs/en:')
        for path in only_zh:
            print('  {}'.format(path))
    return 1


if __name__ == '__main__':
    raise SystemExit(main())

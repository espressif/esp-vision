#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Create one language-level entry page for target-specific documentation."""

import argparse
from pathlib import Path


HTML = """<!doctype html>
<html lang="{language}">
<head>
  <meta charset="utf-8">
  <meta http-equiv="refresh" content="0; url={target}/index.html">
  <meta name="robots" content="noindex">
  <title>ESP-VISION Documentation</title>
</head>
<body>
  <p><a href="{target}/index.html">Open ESP-VISION documentation</a></p>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('build_dir', type=Path)
    parser.add_argument('--target', default='esp32p4')
    args = parser.parse_args()

    for language in ('en', 'zh_CN'):
        target_index = args.build_dir / language / args.target / 'html' / 'index.html'
        if not target_index.is_file():
            raise FileNotFoundError('Missing default target page: {}'.format(target_index))

        landing_dir = args.build_dir / language / 'generic' / 'html'
        landing_dir.mkdir(parents=True, exist_ok=True)
        (landing_dir / 'index.html').write_text(
            HTML.format(language=language, target=args.target),
            encoding='utf-8',
        )
        print('Created {}'.format(landing_dir / 'index.html'))

    return 0


if __name__ == '__main__':
    raise SystemExit(main())

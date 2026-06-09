#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Verify the public language entry URLs produced by ESP-Docs deployment."""

import argparse
import re
import time
import urllib.error
import urllib.request
from pathlib import Path


LINK_PATTERN = re.compile(
    r'^\[document \w+\]\[(en|zh_CN)(?:_(esp32p4))?\]\s+(\S+)$'
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('url_log', type=Path)
    parser.add_argument('--attempts', type=int, default=6)
    parser.add_argument('--delay', type=int, default=5)
    args = parser.parse_args()

    urls = {}
    for line in args.url_log.read_text(encoding='utf-8').splitlines():
        match = LINK_PATTERN.match(line)
        if match:
            language, target, url = match.groups()
            urls[(language, target or 'entry')] = url

    required = {
        ('en', 'entry'),
        ('en', 'esp32p4'),
        ('zh_CN', 'entry'),
        ('zh_CN', 'esp32p4'),
    }
    missing = required - set(urls)
    if missing:
        raise RuntimeError(
            'Missing deployment URLs in deploy log: {}'.format(
                ', '.join('{}_{}'.format(*item) for item in sorted(missing))
            )
        )

    for (language, page), url in sorted(urls.items()):
        last_status = None
        for attempt in range(1, args.attempts + 1):
            try:
                with urllib.request.urlopen(url, timeout=15) as response:
                    last_status = response.status
                    response.read(1)
                if 200 <= last_status < 400:
                    print('{} {} is reachable: {}'.format(language, page, url))
                    break
            except urllib.error.HTTPError as error:
                last_status = error.code
            except urllib.error.URLError as error:
                last_status = str(error.reason)

            if attempt < args.attempts:
                time.sleep(args.delay)
        else:
            raise RuntimeError(
                '{} {} returned {} after deployment: {}. Check the deployment '
                'server user, path, URL base, and project web-root mapping.'.format(
                    language, page, last_status, url
                )
            )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

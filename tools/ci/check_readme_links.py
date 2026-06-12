#!/usr/bin/env python
#
# Checks that all links in the markdown files are valid
#
# Adapted from esp-iot-solution tools/ci/check_readme_links.py
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
#

import argparse
import concurrent.futures
import os
import re
import sys
import urllib.error
import urllib.request
from collections import defaultdict, namedtuple
from pathlib import Path
from typing import List

# Directories that contain vendored code, build output, or generated docs.
EXCLUDE_DIR_PARTS = {'.git', '.cache', 'build', 'lib', 'managed_components', 'esp-iot-solution'}
EXCLUDE_DIR_RELPATHS = ['docs/_build']

# Some links are not accessible from the company network or from scripted CI
# requests, although they are valid in a browser.
EXCLUDE_URL_LIST: List[str] = [
    'https://universe.roboflow.com/machine-learning-2s6qy/oxford-iiit-pet-uvpd0',
]
EXCLUDE_URL_PREFIX_LIST: List[str] = []

REQUEST_HEADERS = {
    'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome Safari',
}

Link = namedtuple('Link', ['file', 'url'])


class ReadmeLinkError(Exception):
    def __init__(self, file: str, url: str) -> None:
        self.file = file
        self.url = url


class RelativeLinkError(ReadmeLinkError):
    def __str__(self) -> str:
        return 'Relative link error, file - {} not found, linked from {}'.format(self.url, self.file)


class UrlLinkError(ReadmeLinkError):
    def __init__(self, file: str, url: str, error_code: str):
        self.error_code = error_code
        super().__init__(file, url)

    def __str__(self) -> str:
        files = [str(f) for f in self.file]
        return 'URL error, url - {} in files - {} is not accessible, request returned {}'.format(self.url, ', '.join(files), self.error_code)


# we do not want a failed test just due to bad network conditions, for non 404 errors we simply print a warning
def check_url(url: str, files: str, timeout: float) -> None:
    try:
        request = urllib.request.Request(url, headers=REQUEST_HEADERS)
        with urllib.request.urlopen(request, timeout=timeout):
            return
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise UrlLinkError(files, url, str(e))
        else:
            print('Unable to access {}, err = {}'.format(url, str(e)))
    except Exception as e:
        print('Unable to access {}, err = {}'.format(url, str(e)))


def check_web_links(web_links: defaultdict) -> List:

    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
        errors = []
        future_to_url = {executor.submit(check_url, url, files, timeout=30): (url, files) for url, files in web_links.items()}
        for future in concurrent.futures.as_completed(future_to_url):
            try:
                future.result()
            except UrlLinkError as e:
                errors.append(e)

        return errors


def check_file_links(file_links: List) -> List:
    errors = []

    for link in file_links:
        link_path = link.file.parent / link.url

        if not Path.exists(link_path):
            errors.append(RelativeLinkError(link.file, link.url))

    print('Found {} errors with relative links'.format(len(errors)))
    return errors


def get_project_root() -> Path:
    root = os.getenv('CI_PROJECT_DIR')
    if root is not None:
        return Path(root)
    return Path(__file__).resolve().parents[2]


def get_md_links(root: Path) -> List:
    MD_LINK_RE = r'\[.+?\]\((.+?)(#.+)?\)'

    exclude_paths = [root / rel for rel in EXCLUDE_DIR_RELPATHS]
    links = []

    for path in root.rglob('*.md'):
        rel_parts = path.relative_to(root).parts
        if any(part in EXCLUDE_DIR_PARTS for part in rel_parts[:-1]):
            continue
        if any(path.is_relative_to(excluded) for excluded in exclude_paths):
            continue

        with path.open(encoding='utf8') as f:
            content = f.read()

        for url in re.findall(MD_LINK_RE, content):
            link = Link(path, url[0].lstrip())
            # Ignore in-page anchors and mail links
            if not link.url.startswith(('#', 'mailto:')):
                links.append(link)

    return links


def check_readme_links(args: argparse.Namespace) -> int:

    links = get_md_links(get_project_root())
    print('Found {} links'.format(len(links)))

    errors = []

    web_links = defaultdict(list)
    file_links = []

    # Sort links into file and web links
    for link in links:
        if link.url.startswith('http'):
            web_links[link.url].append(link.file)
        else:
            file_links.append(link)

    for url in EXCLUDE_URL_LIST:
        web_links.pop(url, None)
    for url in list(web_links.keys()):
        if any(url.startswith(prefix) for prefix in EXCLUDE_URL_PREFIX_LIST):
            web_links.pop(url)

    errors.extend(check_file_links(file_links))

    if not args.skip_weburl:
        errors.extend(check_web_links(web_links))

    print('Found {} errors:'.format(len(errors)))
    for e in errors:
        print(e)

    return 1 if len(errors) > 0 else 0


if __name__ == '__main__':

    parser = argparse.ArgumentParser(description='check_readme_links.py: Checks for dead links in markdown files', prog='check_readme_links.py')
    parser.add_argument('--skip-weburl', '-w', action='store_true', help='Skip checking of web URLs, only check links to local files')
    args = parser.parse_args()

    sys.exit(check_readme_links(args))

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Publish the already checked Expert ZIP and attach immutable GitLab release links."""
import argparse
import io
import json
import os
from pathlib import Path
import re
from urllib.error import HTTPError
from urllib.parse import quote
from urllib.request import Request
import zipfile

from release import Registry, publish, sha256

PACKAGE = 'esp-vision-workbuddy-expert'


def checked_files(directory, tag, commit):
    receipts = list(directory.glob('*.zip.receipt.json'))
    if (len(receipts) != 1 or not re.fullmatch(r'[a-f0-9]{40}', commit)
            or not re.fullmatch(r'(?:v\d+\.\d+(?:\.\d+)?|\d{4}\.\d{1,2}\.\d{1,2})(?:[-.][0-9A-Za-z.-]+)?', tag)):
        raise ValueError('Expected one Expert receipt and the full source SHA')
    receipt = json.loads(receipts[0].read_bytes())
    build = receipt.get('build', {})
    if (build.get('releaseTag') != tag or build.get('sourceCommit') != commit
            or build.get('pipelineId') is not None or not tag
            or build.get('expertVersion') != receipt.get('expertVersion')
            or build.get('sourceVersion') != receipt.get('sourceVersion')):
        raise ValueError('Expert artifact is not the requested fixed tag/source build')
    version = receipt['expertVersion']
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?', version):
        raise ValueError('Invalid Expert version')
    if len(version) > 20:
        raise ValueError(f'WorkBuddy Expert version must be at most 20 characters (got {len(version)})')
    name = f'{PACKAGE}-{version}.zip'
    if receipt.get('archive') != name or receipts[0].name != name + '.receipt.json':
        raise ValueError('Expert archive filename differs from receipt')
    archive = (directory / name).read_bytes()
    checksum = (directory / (name + '.sha256')).read_bytes()
    if (sha256(archive) != receipt.get('sha256')
            or checksum != f'{sha256(archive)}  {name}\n'.encode()):
        raise ValueError('Expert archive/checksum differs from receipt')
    with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
        entries = bundle.infolist()
        root = 'esp-vision-workbuddy/'
        if (len(entries) > 1000 or sum(entry.file_size for entry in entries) > 64 * 1024 * 1024
                or len({entry.filename for entry in entries}) != len(entries)
                or any(not entry.filename.startswith(root) or '..' in entry.filename.split('/')
                       or '\\' in entry.filename for entry in entries)):
            raise ValueError('Unsafe Expert archive')
        if bundle.testzip() is not None:
            raise ValueError('Expert archive CRC check failed')
        if json.loads(bundle.read(root + 'BUILD-PROVENANCE.json')) != build:
            raise ValueError('Expert embedded source identity differs from receipt')
        manifest = json.loads(bundle.read(root + '.codebuddy-plugin/plugin.json'))
        if manifest['version'] != version:
            raise ValueError('Expert manifest version differs from receipt')
        names = manifest.get('displayName')
        if not isinstance(names, dict) or set(names) != {'en', 'zh'}:
            raise ValueError('Expected bilingual Expert displayName')
        for locale, display_name in names.items():
            if not isinstance(display_name, str) or not display_name.strip() or len(display_name) > 15:
                raise ValueError(f'WorkBuddy Expert displayName.{locale} must be nonblank and at most 15 characters')
        description = manifest.get('displayDescription', {}).get('zh')
        if (not isinstance(description, str) or not 40 <= len(description) <= 50
                or '基于 ESP32' not in description):
            raise ValueError('Expert Chinese description must be 40–50 characters and explicitly state 基于 ESP32')
        keywords = manifest.get('keywords')
        if not isinstance(keywords, list) or 'ESP32' not in keywords:
            raise ValueError('Expert keywords must include ESP32')
        tags = manifest.get('tags')
        if not isinstance(tags, list) or not any(
                isinstance(item, dict) and all(isinstance(item.get(locale), str)
                                               and 'ESP32' in item[locale] for locale in ('zh', 'en'))
                for item in tags):
            raise ValueError('Expert visible bilingual tags must include ESP32')
        actual = {entry.filename[len(root):]: {'bytes': entry.file_size,
                  'sha256': sha256(bundle.read(entry))} for entry in entries}
        inventory = {item['path']: {'bytes': item['bytes'], 'sha256': item['sha256']}
                     for item in receipt['inventory']}
        if actual != inventory or len(inventory) != len(receipt['inventory']):
            raise ValueError('Expert archive inventory differs from receipt')
    # The receipt is written last and binds the archive/checksum cohort.
    return version, {name: archive, name + '.sha256': checksum, receipts[0].name: receipts[0].read_bytes()}


class ExpertRegistry(Registry):
    def file(self, version, name, data=None):
        return self.request(self.package_path(version, name), data)

    @staticmethod
    def package_path(version, name):
        return '/packages/generic/{}/{}/{}'.format(PACKAGE, quote(version, safe=''), quote(name, safe=''))

    def api(self, path, data=None):
        request = Request(self.base + path, data=None if data is None else json.dumps(data).encode(),
                          headers={'JOB-TOKEN': self.token, 'Content-Type': 'application/json'},
                          method='GET' if data is None else 'POST')
        try:
            with self.opener.open(request, timeout=30) as response:
                return json.load(response)
        except HTTPError as error:
            if error.code == 404:
                return None
            raise RuntimeError(f'GitLab release request failed (HTTP {error.code})') from None


def release_links(registry, version, files):
    return [{'name': name, 'url': registry.base + registry.package_path(version, name), 'link_type': 'package'}
            for name in files]


def preflight(registry, tag, commit, version, files):
    tag_info = registry.api('/repository/tags/' + quote(tag, safe=''))
    if not tag_info or tag_info.get('commit', {}).get('id') != commit:
        raise ValueError('Existing Git tag does not point to the requested source commit')
    path = '/releases/' + quote(tag, safe='')
    release = registry.api(path)
    if release is not None and release.get('commit', {}).get('id') != commit:
        raise ValueError('Release tag points to a different source commit')
    existing = [] if release is None else registry.api(path + '/assets/links')
    expected = release_links(registry, version, files)
    for target in expected:
        conflicts = [item for item in existing if item['name'] == target['name'] or item['url'] == target['url']]
        if conflicts and any(item['name'] != target['name'] or item['url'] != target['url'] for item in conflicts):
            raise ValueError('Release already contains a different Expert asset link')
    return path, release, existing, expected


def attach(registry, tag, commit, version, files):
    # Recheck after upload, then create a Release only for an existing verified
    # Git tag. Omitting ref prevents the API from implicitly creating a tag.
    path, release, existing, expected = preflight(registry, tag, commit, version, files)
    if release is None:
        registry.api('/releases', {'tag_name': tag, 'name': tag,
                     'description': f'ESP-VISION {tag}. WorkBuddy Expert source: `{commit}`.'})
        release = registry.api(path)
        if not release or release.get('commit', {}).get('id') != commit:
            raise ValueError('Created Release source differs from the checked tag')
    for target in expected:
        if not any(item['name'] == target['name'] and item['url'] == target['url'] for item in existing):
            registry.api(path + '/assets/links', target)
    actual = registry.api(path + '/assets/links')
    if not all(any(item['name'] == target['name'] and item['url'] == target['url'] for item in actual) for target in expected):
        raise RuntimeError('Release asset link verification failed')


def publish_release(registry, tag, commit, version, files):
    # Shared Knowledge publisher rejects conflicting bytes before any PUT and
    # reads back every upload. No npm or firmware publication is involved.
    preflight(registry, tag, commit, version, files)
    publish(registry, version, files)
    attach(registry, tag, commit, version, files)


def require_publish_ci(env, tag, commit):
    if (env.get('CI_COMMIT_REF_PROTECTED') != 'true'
            or env.get('CI_COMMIT_TAG') != tag or env.get('CI_COMMIT_SHA') != commit):
        raise ValueError('Expert publication requires the exact protected release tag/source')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--publish', action='store_true')
    args = parser.parse_args()
    version, files = checked_files(args.directory, args.tag, args.source_commit)
    if args.publish:
        require_publish_ci(os.environ, args.tag, args.source_commit)
        registry = ExpertRegistry(os.environ['CI_API_V4_URL'], os.environ['CI_PROJECT_ID'], os.environ['CI_JOB_TOKEN'])
        publish_release(registry, args.tag, args.source_commit, version, files)
    print(f'Expert {args.tag}: checked {len(files)} artifacts; published={args.publish}')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Assemble exact firmware snapshots, validate catalogs and prevent history loss."""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import re
import tarfile
from urllib.request import Request, build_opener
from urllib.parse import urlsplit

from release import Registry, StripRedirectCredentials, publish, sha256

LIMIT = 64 * 1024 * 1024
FIELDS = ('firmwareVersion', 'snapshotVersion', 'firmwareSourceCommit', 'contentSourceCommit', 'packSha256')


def identity(snapshot):
    return {key: snapshot[key] for key in FIELDS}


def validate_snapshot(snapshot):
    for key in ('firmwareVersion', 'snapshotVersion'):
        if not isinstance(snapshot.get(key), str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,127}', snapshot[key]):
            raise ValueError('Invalid snapshot version')
    for key in ('firmwareSourceCommit', 'contentSourceCommit'):
        if not re.fullmatch(r'[a-f0-9]{40}', snapshot.get(key, '')):
            raise ValueError('Invalid snapshot commit')
    payload = snapshot['packJson'].encode('utf-8')
    if sha256(payload) != snapshot['packSha256']:
        raise ValueError('Snapshot hash mismatch')
    pack = json.loads(payload)
    if (pack['meta']['version'] != snapshot['firmwareVersion'] or
            pack['meta']['sourceCommit'] != snapshot['contentSourceCommit']):
        raise ValueError('Snapshot content identity mismatch')
    # Historical schema remains validated by its source archive; require the
    # stable structural fields that all supported packs have used.
    if not all(isinstance(pack.get(key), list) for key in ('modules', 'docs', 'examples', 'capabilities')):
        raise ValueError('Invalid snapshot content')
    return snapshot


def validate_catalog(data):
    if len(data) > LIMIT:
        raise ValueError('Catalog exceeds 64 MiB')
    catalog = json.loads(data)
    if catalog.get('schemaVersion') != 1 or not isinstance(catalog.get('snapshots'), list) or not catalog['snapshots']:
        raise ValueError('Invalid catalog schema')
    snapshots = {}
    firmware_commits = {}
    for record in catalog['snapshots']:
        validate_snapshot(record)
        if record['snapshotVersion'] in snapshots:
            raise ValueError('Duplicate snapshot')
        previous = firmware_commits.setdefault(record['firmwareVersion'], record['firmwareSourceCommit'])
        if previous != record['firmwareSourceCommit']:
            raise ValueError('Firmware version maps to conflicting commits')
        snapshots[record['snapshotVersion']] = record
    active = catalog['activeSnapshots']
    if not isinstance(active, dict) or set(active) != {s['firmwareVersion'] for s in snapshots.values()}:
        raise ValueError('Missing active firmware mapping')
    if (not isinstance(catalog.get('latestFirmwareVersion'), str) or catalog['latestFirmwareVersion'] == 'latest'
            or catalog['latestFirmwareVersion'] not in active):
        raise ValueError('Latest firmware must identify an active snapshot')
    for firmware, revision in active.items():
        if revision not in snapshots or snapshots[revision]['firmwareVersion'] != firmware:
            raise ValueError('Invalid active snapshot mapping')
    return catalog


def snapshot_from_pack(payload, firmware_commit=None, revision=None):
    pack = json.loads(payload)
    return validate_snapshot({
        'firmwareVersion': pack['meta']['version'], 'snapshotVersion': revision or pack['meta']['version'],
        'firmwareSourceCommit': firmware_commit or pack['meta']['sourceCommit'],
        'contentSourceCommit': pack['meta']['sourceCommit'], 'packSha256': sha256(payload),
        'packJson': payload.decode('utf-8'),
    })


def archive_catalog(data):
    if len(data) > LIMIT:
        raise ValueError('Archive too large')
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tar:
        names = set()
        total = 0
        for member in tar.getmembers():
            total += member.size
            if total > 128 * 1024 * 1024 or member.name in names or not member.name.startswith('package/') or '..' in member.name.split('/') or not (member.isfile() or member.isdir()):
                raise ValueError('Unsafe npm archive')
            names.add(member.name)
        def read(name):
            member = tar.getmember('package/' + name)
            if not member.isfile() or member.size > LIMIT:
                raise ValueError('Invalid npm data')
            return tar.extractfile(member).read(LIMIT + 1)
        if 'package/data/mcp-catalog.json' in names:
            return validate_catalog(read('data/mcp-catalog.json'))
        snapshot = snapshot_from_pack(read('data/mcp-pack.json'))
        return {'schemaVersion': 1, 'latestFirmwareVersion': snapshot['firmwareVersion'], 'snapshots': [snapshot],
                'activeSnapshots': {snapshot['firmwareVersion']: snapshot['snapshotVersion']}}


def preserve_history(previous, current):
    old = {s['snapshotVersion']: s for s in previous['snapshots']}
    new = {s['snapshotVersion']: s for s in current['snapshots']}
    for version, snapshot in old.items():
        if version not in new or snapshot != new[version]:
            raise ValueError('Historical snapshot removed or changed: ' + version)


def download(source):
    url = source['url']
    parsed = urlsplit(url)
    if parsed.scheme != 'https' or parsed.username or parsed.password:
        raise ValueError('Snapshot source must use HTTPS without credentials')
    headers = {}
    # CI credentials may only be sent to this project's GitLab API.
    api = os.environ.get('CI_API_V4_URL', '').rstrip('/')
    project = os.environ.get('CI_PROJECT_ID', '')
    if api and project and url.startswith(api + '/projects/' + project + '/packages/generic/'):
        headers['JOB-TOKEN'] = os.environ.get('CI_JOB_TOKEN', '')
    with build_opener(StripRedirectCredentials()).open(Request(url, headers=headers), timeout=60) as response:
        data = response.read(LIMIT + 1)
    if len(data) > LIMIT:
        raise ValueError('Snapshot source too large')
    integrity = source.get('integrity', '')
    expected = 'sha512-' + base64.b64encode(hashlib.sha512(data).digest()).decode()
    if integrity != expected:
        raise ValueError('Snapshot source integrity mismatch')
    return data


def assemble(lock):
    snapshots = []
    for entry in lock['snapshots']:
        data = download(entry['source'])
        if entry['source']['kind'] == 'npm':
            matches = [s for s in archive_catalog(data)['snapshots'] if s['snapshotVersion'] == entry['snapshotVersion']]
            if len(matches) != 1:
                raise ValueError('Snapshot missing from source archive')
            snapshot = matches[0]
        elif entry['source']['kind'] == 'snapshot':
            snapshot = validate_snapshot(json.loads(data))
        else:
            raise ValueError('Unknown snapshot source kind')
        if identity(snapshot) != identity(entry):
            raise ValueError('Locked snapshot differs from archived identity')
        snapshots.append(snapshot)
    payload = encode({'schemaVersion': 1, 'latestFirmwareVersion': lock['latestFirmwareVersion'],
                      'activeSnapshots': lock['activeSnapshots'], 'snapshots': snapshots})
    validate_catalog(payload)
    return payload


def encode(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')) + '\n').encode('utf-8')


class SnapshotRegistry(Registry):
    def file(self, version, name, data=None):
        from urllib.parse import quote
        return self.request('/packages/generic/esp-vision-knowledge-snapshot/{}/{}'.format(quote(version, safe=''), name), data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    build = sub.add_parser('assemble')
    build.add_argument('--lock', type=Path, required=True)
    build.add_argument('--output', type=Path, required=True)
    check = sub.add_parser('check')
    check.add_argument('--catalog', type=Path, required=True)
    check.add_argument('--previous-npm', type=Path)
    snapshot = sub.add_parser('snapshot')
    snapshot.add_argument('--pack', type=Path, required=True)
    snapshot.add_argument('--version', required=True)
    snapshot.add_argument('--source-commit', required=True)
    snapshot.add_argument('--output', type=Path, required=True)
    snapshot.add_argument('--publish', action='store_true')
    args = parser.parse_args()
    if args.command == 'assemble':
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(assemble(json.loads(args.lock.read_bytes())))
    elif args.command == 'check':
        current = validate_catalog(args.catalog.read_bytes())
        if args.previous_npm:
            preserve_history(archive_catalog(args.previous_npm.read_bytes()), current)
    else:
        record = snapshot_from_pack(args.pack.read_bytes())
        if record['firmwareVersion'] != args.version or record['firmwareSourceCommit'] != args.source_commit:
            raise ValueError('Snapshot does not match qualified firmware tag/commit')
        payload = encode(record)
        args.output.mkdir(parents=True, exist_ok=True)
        files = {'snapshot.json': payload, 'snapshot.sha256': (sha256(payload) + '\n').encode()}
        for name, data in files.items():
            (args.output / name).write_bytes(data)
        if args.publish:
            if (os.environ.get('CI_COMMIT_TAG') != args.version or os.environ.get('CI_COMMIT_SHA') != args.source_commit or
                    os.environ.get('CI_COMMIT_REF_PROTECTED') != 'true'):
                raise ValueError('Snapshot publication requires the protected qualified firmware tag')
            registry = SnapshotRegistry(os.environ['CI_API_V4_URL'], os.environ['CI_PROJECT_ID'], os.environ['CI_JOB_TOKEN'])
            publish(registry, args.version, files)
        print(json.dumps({**identity(record), 'source': {'kind': 'snapshot',
                         'integrity': 'sha512-' + base64.b64encode(hashlib.sha512(payload).digest()).decode()}}))


if __name__ == '__main__':
    main()

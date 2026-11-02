# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Verify platform exports and attach unchanged bytes to a GitHub tag Release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request
import zipfile


def verify(directory, commit):
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Expected full source commit')
    archives = sorted(directory.glob('*.zip'))
    expected = {'esp-vision-workbuddy-expert-', 'esp-vision-qoderwork-', 'esp-vision-cursor-'}
    if len(archives) != 3 or any(sum(p.name.startswith(prefix) for p in archives) != 1 for prefix in expected):
        raise ValueError('Expected exactly one ZIP for each platform')
    for archive in archives:
        checksum = Path(str(archive) + '.sha256')
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if checksum.read_text().strip() != f'{digest}  {archive.name}':
            raise ValueError(f'Checksum mismatch: {archive.name}')
        with zipfile.ZipFile(archive) as bundle:
            if bundle.testzip() is not None:
                raise ValueError('ZIP CRC mismatch')
            entries = bundle.namelist()
            provenance = [n for n in entries if n.endswith('/BUILD-PROVENANCE.json')]
            if len(provenance) != 1:
                raise ValueError('Missing unique build provenance')
            identity = json.loads(bundle.read(provenance[0]))
            if identity.get('sourceCommit') != commit:
                raise ValueError(f'Source mismatch: {archive.name}')
    for receipt in directory.glob('*.zip.receipt.json'):
        metadata = json.loads(receipt.read_text())
        archive = directory / receipt.name.removesuffix('.receipt.json')
        if metadata.get('sha256') != hashlib.sha256(archive.read_bytes()).hexdigest():
            raise ValueError('Receipt checksum mismatch')
        if metadata.get('build', {}).get('sourceCommit') != commit:
            raise ValueError('Receipt source mismatch')
    return sorted(p for p in directory.iterdir() if p.is_file() and
                  (p.name.endswith('.zip') or p.name.endswith('.zip.sha256') or p.name.endswith('.zip.receipt.json')))


def publish(files, tag, commit):
    if not re.fullmatch(r'(?:v\d+\.\d+(?:\.\d+)?|\d{4}\.\d{1,2}\.\d{1,2})(?:[-.][0-9A-Za-z.-]+)?', tag):
        raise ValueError('Unsupported release tag')
    repository = os.environ['GITHUB_REPOSITORY']
    base = os.environ.get('GITHUB_API_URL', 'https://api.github.com') + '/repos/' + repository
    headers = {'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
               'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'}

    def request(url, data=None, content_type='application/json'):
        extra = dict(headers)
        if data is not None:
            extra['Content-Type'] = content_type
        with urllib.request.urlopen(urllib.request.Request(url, data=data, headers=extra), timeout=120) as response:
            return json.load(response)

    try:
        release = request(base + '/releases/tags/' + urllib.parse.quote(tag, safe=''))
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
        release = request(base + '/releases', json.dumps({'tag_name': tag, 'target_commitish': commit,
                          'name': tag, 'draft': True, 'body': 'ESP-VISION release packages.'}).encode())
    # Preserve existing notes, draft/published state and unrelated assets.
    assets = []
    page = 1
    while True:
        batch = request(base + f"/releases/{release['id']}/assets?per_page=100&page={page}")
        assets.extend(batch)
        if len(batch) < 100:
            break
        page += 1
    existing = {asset['name']: asset for asset in assets}
    # Preflight every collision before uploading anything; never clobber a release.
    for path in files:
        if path.name in existing:
            asset = existing[path.name]
            extra = dict(headers, Accept='application/octet-stream')
            with urllib.request.urlopen(urllib.request.Request(asset['url'], headers=extra), timeout=120) as response:
                actual = response.read()
            if hashlib.sha256(actual).digest() != hashlib.sha256(path.read_bytes()).digest():
                raise ValueError(f'Existing release asset differs: {path.name}')
    upload = release['upload_url'].split('{', 1)[0]
    for path in files:
        if path.name not in existing:
            request(upload + '?name=' + urllib.parse.quote(path.name, safe=''), path.read_bytes(),
                    'application/zip' if path.suffix == '.zip' else 'application/octet-stream')
        print(f"Release asset verified/attached: {path.name}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--tag')
    parser.add_argument('--publish', action='store_true')
    args = parser.parse_args()
    files = verify(args.directory, args.source_commit)
    if args.publish:
        publish(files, args.tag or '', args.source_commit)
    else:
        print(f'Verified {len(files)} release files; no publication requested')

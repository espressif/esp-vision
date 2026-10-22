#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Bind validated knowledge bytes to an immutable main-repository server image."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import tarfile
from urllib.error import HTTPError
from urllib.parse import quote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener


PACKAGE_NAME = 'esp-vision-mcp-knowledge'
VERSION = re.compile(r'[A-Za-z0-9][A-Za-z0-9._-]{0,127}')
DIGEST = re.compile(r'sha256:[a-f0-9]{64}')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def npm_identity(npm_bytes, pack_bytes, version, source_commit):
    if not npm_bytes or len(npm_bytes) > 64 * 1024 * 1024:
        raise ValueError('Knowledge npm tarball size is invalid')
    with tarfile.open(fileobj=io.BytesIO(npm_bytes), mode='r:gz') as archive:
        members = archive.getmembers()
        if sum(member.size for member in members) > 128 * 1024 * 1024:
            raise ValueError('Knowledge npm archive expands beyond the allowed size')
        names = [member.name for member in members]
        if len(names) != len(set(names)):
            raise ValueError('Duplicate npm archive entries')
        for member in members:
            if (not member.name.startswith('package/') or '..' in member.name.split('/')
                    or member.issym() or member.islnk()
                    or not (member.isfile() or member.isdir())):
                raise ValueError('Unsafe npm archive entry')
        def read(name, limit):
            member = archive.getmember('package/' + name)
            if not member.isfile() or member.size > limit:
                raise ValueError('Invalid npm archive metadata')
            return archive.extractfile(member).read(limit + 1)
        package = json.loads(read('package.json', 64 * 1024))
        embedded = read('data/mcp-pack.json', 32 * 1024 * 1024)
        provenance = json.loads(read('data/provenance.json', 64 * 1024))
    if package.get('name') != '@esp-vision/knowledge-mcp' or package.get('private'):
        raise ValueError('Unexpected Knowledge npm package')
    package_version = package.get('version', '')
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?', package_version):
        raise ValueError('Invalid Knowledge npm version')
    expected = {'schemaVersion': 1, 'packageName': package['name'],
                'packageVersion': package_version, 'packVersion': version,
                'sourceCommit': source_commit, 'packSha256': sha256(pack_bytes)}
    if embedded != pack_bytes or any(provenance.get(key) != value for key, value in expected.items()):
        raise ValueError('Knowledge npm provenance does not match the release')
    return {'name': package['name'], 'version': package_version,
            'file': 'knowledge-mcp.tgz', 'sha256': sha256(npm_bytes), 'bytes': len(npm_bytes)}


def descriptor(pack_bytes, version, source_commit, image_repository, image_digest, npm_bytes=None):
    if not VERSION.fullmatch(version) or not re.fullmatch(r'[a-f0-9]{40}', source_commit):
        raise ValueError('Invalid release identity')
    pack = json.loads(pack_bytes)
    if pack.get('meta', {}).get('version') != version or pack['meta'].get('sourceCommit') != source_commit:
        raise ValueError('Knowledge version/source does not match the release')
    if not DIGEST.fullmatch(image_digest):
        raise ValueError('An immutable SHA-256 image digest is required')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.:/_-]*', image_repository) or '@' in image_repository or ':' in image_repository.rsplit('/', 1)[-1]:
        raise ValueError('Invalid image repository')
    manifest = {
        'schemaVersion': 2 if npm_bytes is not None else 1, 'version': version, 'sourceCommit': source_commit,
        'knowledge': {'file': 'mcp-pack.json', 'sha256': sha256(pack_bytes), 'bytes': len(pack_bytes)},
        'image': {'reference': image_repository + '@' + image_digest, 'digest': image_digest},
    }
    if npm_bytes is not None:
        manifest['npm'] = npm_identity(npm_bytes, pack_bytes, version, source_commit)
    return manifest


class StripRedirectCredentials(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, new_url):
        redirected = super().redirect_request(req, fp, code, msg, headers, new_url)
        if redirected is not None:
            old, new = urlsplit(req.full_url), urlsplit(new_url)
            if (old.scheme, old.netloc) != (new.scheme, new.netloc):
                for mapping in (redirected.headers, redirected.unredirected_hdrs):
                    for key in list(mapping):
                        if key.lower() in ('job-token', 'private-token', 'deploy-token', 'authorization'):
                            mapping.pop(key, None)
        return redirected


class Registry:
    def __init__(self, api_url, project_id, token):
        if urlsplit(api_url).scheme != 'https':
            raise ValueError('GitLab registry API must use HTTPS')
        self.base = api_url.rstrip('/') + '/projects/' + quote(str(project_id), safe='')
        self.token = token
        self.opener = build_opener(StripRedirectCredentials())

    def request(self, path, data=None):
        req = Request(self.base + path, data=data, headers={'JOB-TOKEN': self.token},
                      method='GET' if data is None else 'PUT')
        try:
            with self.opener.open(req, timeout=30) as response:
                return response.read()
        except HTTPError as error:
            if error.code == 404:
                return None
            raise RuntimeError('GitLab package request failed (HTTP {})'.format(error.code)) from None

    def file(self, version, name, data=None):
        path = '/packages/generic/{}/{}/{}'.format(PACKAGE_NAME, quote(version, safe=''), name)
        return self.request(path, data)


def release_files(pack_bytes, manifest, npm_bytes=None):
    encoded = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    # The descriptor checksum is the last publication marker. Consumers never
    # select an incomplete package by listing or a mutable latest pointer.
    files = {
        'mcp-pack.json': pack_bytes,
        'mcp-pack.sha256': (sha256(pack_bytes) + '\n').encode(),
        'mcp-release.json': encoded,
        'mcp-release.sha256': (sha256(encoded) + '\n').encode(),
    }
    if manifest.get('schemaVersion') == 2:
        identity = npm_identity(npm_bytes, pack_bytes, manifest['version'], manifest['sourceCommit'])
        if identity != manifest.get('npm'):
            raise ValueError('Knowledge npm identity differs from descriptor')
        # Keep the descriptor checksum as the final publication marker.
        files = {'knowledge-mcp.tgz': npm_bytes, **files}
    elif npm_bytes is not None:
        raise ValueError('Legacy descriptors cannot carry npm artifacts')
    return files


def publish(registry, version, files):
    existing = {name: registry.file(version, name) for name in files}
    # Check the whole existing cohort before writing anything. Retrying a
    # partial identical publication is safe; a changed image/version is not.
    for name, data in files.items():
        if existing[name] is not None and existing[name] != data:
            raise ValueError('Release version already contains different {}'.format(name))
    for name, data in files.items():
        if existing[name] is None:
            registry.file(version, name, data)
        if registry.file(version, name) != data:
            raise RuntimeError('Published file verification failed: {}'.format(name))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', type=Path, required=True)
    parser.add_argument('--npm-tarball', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--image-repository', required=True)
    parser.add_argument('--image-digest-file', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--publish', action='store_true')
    args = parser.parse_args()
    data = args.pack.read_bytes()
    manifest = descriptor(data, args.version, args.source_commit, args.image_repository,
                          args.image_digest_file.read_text().strip(), args.npm_tarball.read_bytes())
    files = release_files(data, manifest, args.npm_tarball.read_bytes())
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in files.items():
        (args.output / name).write_bytes(payload)
    if args.publish:
        for key in ('CI_API_V4_URL', 'CI_PROJECT_ID', 'CI_JOB_TOKEN'):
            if not os.environ.get(key):
                raise ValueError('Missing registry CI configuration')
        publish(Registry(os.environ['CI_API_V4_URL'], os.environ['CI_PROJECT_ID'],
                         os.environ['CI_JOB_TOKEN']), args.version, files)
    print('MCP release {}: {}'.format(args.version, manifest['image']['reference']))


if __name__ == '__main__':
    main()

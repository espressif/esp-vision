# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Immutable release manifests and the verified mutable publication pointer."""
import copy
import hashlib
import json
from pathlib import Path
import re
import urllib.request

ROOT_URL = 'https://dl.espressif.com/AE/esp-vision/'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def valid_version(version):
    return isinstance(version, str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', version) and not version.startswith('device-core-v')


def manifest_url(version):
    require(valid_version(version), 'Invalid firmware publication version')
    return ROOT_URL + f'releases/{version}/manifest.json'


def validate_manifest(manifest, version=None):
    require(manifest['schemaVersion'] == 1 and valid_version(manifest['version']), 'Invalid manifest schema/version')
    require(version is None or manifest['version'] == version, 'Manifest version mismatch')
    require(re.fullmatch(r'[0-9a-f]{40}', manifest['sourceCommit']) is not None, 'Invalid manifest source commit')
    require(type(manifest['pipelineId']) is int and manifest['pipelineId'] > 0, 'Invalid manifest pipeline ID')
    require(manifest['layout'] in ('legacy', 'single-bin'), 'Invalid manifest layout')
    entries = manifest['releases']
    require(isinstance(entries, list) and bool(entries), 'Manifest has no releases')
    boards = [entry['board'] for entry in entries]
    require(all(isinstance(board, str) and re.fullmatch(r'[A-Za-z0-9_-]+', board) for board in boards)
            and len(set(boards)) == len(boards), 'Invalid/duplicate manifest boards')
    require(all(entry['version'] == manifest['version'] and entry.get('sourceCommit') == manifest['sourceCommit']
                and type(entry['capabilities']['wifi']) is bool
                for entry in entries), 'Manifest release identity/capabilities mismatch')
    website = manifest['website']
    require(website['release'] == manifest['version'] and isinstance(website['schemaVersion'], str)
            and re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', website['schemaVersion']) is not None
            and isinstance(website['boards'], list), 'Invalid website snapshot')
    require(all(board['board'] in boards for board in website['boards'] if board.get('flashable') is True),
            'Flashable website board is absent from manifest')
    for model in website.get('models', []):
        require(model['downloadUrl'] == 'https://raw.githubusercontent.com/espressif/esp-vision/'
                + manifest['sourceCommit'] + '/' + model['path'], 'Model URL is not pinned to release source')
    mcp = manifest['mcp']
    if mcp is not None:
        require(mcp['version'] == manifest['version'] and mcp['sourceCommit'] == manifest['sourceCommit']
                and mcp['url'] == ROOT_URL + f"bin/mcp/mcp-pack-{manifest['version']}.json"
                and type(mcp['bytes']) is int and mcp['bytes'] > 0
                and re.fullmatch(r'[0-9a-f]{64}', mcp['sha256']) is not None, 'Invalid MCP descriptor')
    return boards


def fetch_public(url):
    require(url.startswith(ROOT_URL), 'Unexpected publication URL origin')
    request = urllib.request.Request(url, headers={'Cache-Control': 'no-cache'})
    with urllib.request.urlopen(request, timeout=60) as response:
        require(response.geturl().startswith(ROOT_URL), 'Unexpected publication redirect')
        return response.read()


def parse_config(data):
    try:
        import tomllib
        return tomllib.loads(data.decode('utf-8'))
    except ImportError:
        import rtoml
        return rtoml.loads(data.decode('utf-8'))


def previous_publication(data, fetch=fetch_public):
    """Only successfully published, hash-bound manifest records become history.

    A missing publication table is a legacy config, not a bootstrap error. A
    malformed pointer, failed fetch or bad historical manifest fails closed.
    Legacy website versions are retained separately for binary downloads only.
    """
    previous = parse_config(data)
    publication = previous.get('publication')
    if publication is None:
        return previous, []
    require(publication['schemaVersion'] == 1, 'Unsupported previous publication schema')
    records = publication['releases']
    require(isinstance(records, list) and bool(records), 'Published history is empty')
    versions = set()
    current = None
    for record in records:
        version = record['version']
        require(valid_version(version) and version not in versions, 'Invalid/duplicate publication history')
        versions.add(version)
        require(record['manifestUrl'] == manifest_url(version)
                and re.fullmatch(r'[0-9a-f]{64}', record['manifestSha256']) is not None,
                'Invalid historical manifest reference')
        raw = fetch(record['manifestUrl'])
        require(sha256(raw) == record['manifestSha256'], 'Historical manifest SHA-256 mismatch')
        manifest = json.loads(raw)
        boards = validate_manifest(manifest, version)
        require(isinstance(record['boards'], list) and bool(record['boards'])
                and len(set(record['boards'])) == len(record['boards'])
                and set(record['boards']) == set(boards), 'Historical board membership mismatch')
        if version == publication['version']:
            require(record['manifestUrl'] == publication['manifestUrl']
                    and record['manifestSha256'] == publication['manifestSha256']
                    and manifest['sourceCommit'] == publication['sourceCommit']
                    and manifest['pipelineId'] == publication['pipelineId'], 'Previous current pointer mismatch')
            current = manifest
    require(current is not None, 'Current publication is missing from history')
    return previous, copy.deepcopy(records)


def write_publication(directory, config, entries, version, source_commit, pipeline_id,
                      previous_bytes, fetch=fetch_public, layout='legacy', allow_missing_mcp=False):
    require(re.fullmatch(r'[0-9a-f]{40}', source_commit) is not None, 'Source commit must be full HEAD SHA')
    require(type(pipeline_id) is int and pipeline_id > 0, 'Pipeline ID must be positive')
    previous, history = previous_publication(previous_bytes, fetch)
    require(all(record['version'] != version for record in history), 'Published firmware version is immutable')
    directory = Path(directory)
    website = copy.deepcopy(config['website'])
    versions = [version] + [record['version'] for record in history]
    versions += previous.get('website', {}).get('releases', [])
    website['releases'] = list(dict.fromkeys(v for v in versions if valid_version(v)))
    for model in website.get('models', []):
        model['downloadUrl'] = 'https://raw.githubusercontent.com/espressif/esp-vision/' + source_commit + '/' + model['path']
    mcp_path = directory / 'mcp' / f'mcp-pack-{version}.json'
    mcp = None
    if mcp_path.is_file():
        data = mcp_path.read_bytes()
        pack = json.loads(data)
        require(pack['meta']['version'] == version and pack['meta']['sourceCommit'] == source_commit,
                'MCP build identity mismatch')
        mcp = dict(url=ROOT_URL + f'bin/mcp/{mcp_path.name}', bytes=len(data), sha256=sha256(data),
                   version=version, sourceCommit=source_commit)
    else:
        require(allow_missing_mcp, 'MCP pack missing; explicitly allow absence or regenerate it')
        require(not (directory / 'mcp' / 'latest.json').exists(), 'Missing MCP cannot retain a latest alias')
    manifest = dict(schemaVersion=1, version=version, sourceCommit=source_commit, pipelineId=pipeline_id,
                    layout=layout, releases=entries, website=website, mcp=mcp)
    boards = validate_manifest(manifest, version)
    raw = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode('utf-8')
    destination = directory / 'releases' / version / 'manifest.json'
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(raw)
    record = dict(version=version, manifestUrl=manifest_url(version), manifestSha256=sha256(raw), boards=boards)
    config['website'] = website
    config['publication'] = dict(schemaVersion=1, version=version, sourceCommit=source_commit,
                                 pipelineId=pipeline_id, manifestUrl=record['manifestUrl'],
                                 manifestSha256=record['manifestSha256'], previousConfigSha256=sha256(previous_bytes),
                                 releases=[record, *history])
    return manifest

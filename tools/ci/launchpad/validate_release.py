# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Reject incomplete release packages before they reach the additive uploader."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import re

sys.path.insert(0, str(Path(__file__).resolve().parent))
from publication import manifest_url, sha256, validate_manifest  # noqa: E402

ROOT_URL = 'https://dl.espressif.com/AE/esp-vision/'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_digest(data, descriptor, label):
    require(len(data) == descriptor['bytes'], f'{label}: size mismatch')
    require(hashlib.sha256(data).hexdigest() == descriptor['sha256'], f'{label}: SHA-256 mismatch')


def image_bytes(directory, descriptor):
    url = descriptor['url']
    prefix = ROOT_URL + 'bin/'
    require(url.startswith(prefix), f'Unexpected published image URL: {url}')
    name = url[len(prefix):]
    require(name and Path(name).name == name and name not in ('.', '..'), 'Image URL must name one binary')
    data = (directory / name).read_bytes()
    check_digest(data, descriptor, name)
    return data, name


def validate_release(directory, tag, config, source_commit=None, pipeline_id=None, require_publication=False):
    """Validate the flat binaries/ source package and its public URL contract.

    The uploader keeps config.toml and firmware/ at the public root and places
    the flat binary files in bin/. It must not relocate firmware/ under bin/.
    Only this release's metadata is required; historical versions stay additive.
    """
    directory = Path(directory)
    require(tag and Path(tag).name == tag and tag not in ('.', '..'), 'Invalid firmware tag')
    require(not tag.startswith('device-core-v'), 'SDK tags are not firmware releases')
    require(config['website']['release'] == tag, 'Config release differs from pipeline tag')
    require(config['firmware_images_url'] == ROOT_URL + 'bin/', 'Incorrect binary URL base')
    boards = {}
    for app in config['supported_apps']:
        boards.update(config[app]['image'])
    require(bool(boards), 'No published boards')
    publication = config.get('publication')
    require(not require_publication or publication is not None, 'Publication pointer required')
    manifest = None
    if publication is not None:
        require(publication['schemaVersion'] == 1 and publication['version'] == tag
                and publication['manifestUrl'] == manifest_url(tag)
                and re.fullmatch(r'[0-9a-f]{64}', publication['previousConfigSha256']) is not None,
                'Invalid publication pointer')
        raw = (directory / 'releases' / tag / 'manifest.json').read_bytes()
        require(sha256(raw) == publication['manifestSha256'], 'Manifest SHA-256 mismatch')
        manifest = json.loads(raw)
        manifest_boards = validate_manifest(manifest, tag)
        require(set(manifest_boards) == set(boards), 'Manifest/config board membership differs')
        require(publication['sourceCommit'] == manifest['sourceCommit']
                and publication['pipelineId'] == manifest['pipelineId'], 'Publication build identity mismatch')
        require(source_commit is None or source_commit == manifest['sourceCommit'], 'Unexpected source commit')
        require(pipeline_id is None or pipeline_id == manifest['pipelineId'], 'Unexpected source pipeline')
        require(config['website'] == manifest['website'], 'Website snapshot differs from manifest')
        records = publication['releases']
        require(bool(records) and records[0]['version'] == tag
                and records[0]['manifestUrl'] == publication['manifestUrl']
                and records[0]['manifestSha256'] == publication['manifestSha256']
                and len(set(records[0]['boards'])) == len(boards)
                and set(records[0]['boards']) == set(boards), 'Current history record mismatch')
        require(len({record['version'] for record in records}) == len(records), 'Duplicate history records')
        mcp = manifest['mcp']
        if mcp is not None:
            mcp_bytes = (directory / 'mcp' / Path(mcp['url']).name).read_bytes()
            check_digest(mcp_bytes, mcp, 'MCP pack')
            meta = json.loads(mcp_bytes)['meta']
            require(meta['version'] == tag and meta['sourceCommit'] == manifest['sourceCommit'],
                    'MCP content build identity mismatch')
            require((directory / 'mcp/latest.json').read_bytes() == (directory / 'mcp' / Path(mcp['url']).name).read_bytes(),
                    'MCP latest alias differs from this release')
        else:
            require(not (directory / 'mcp/latest.json').exists(), 'Absent MCP cannot retain latest alias')
    for board, full_name in boards.items():
        require(Path(board).name == board and board not in ('.', '..'), 'Invalid board name')
        metadata_path = directory / 'firmware' / board / (tag + '.json')
        if manifest is None:
            entry = json.loads(metadata_path.read_text())
        else:
            entry = next(item for item in manifest['releases'] if item['board'] == board)
            require(entry.get('sourceCommit') == manifest['sourceCommit'], f'{board}: app build source mismatch')
            if manifest['layout'] == 'legacy':
                require(json.loads(metadata_path.read_text()) == entry, f'{board}: compatibility metadata differs')
                require('source' not in entry['image'], f'{board}: mixed publication layout')
            else:
                require(not metadata_path.exists() and 'source' in entry['image'], f'{board}: mixed publication layout')
                require(not (directory / f'esp-vision-{board}-{tag}-app.bin').exists(), f'{board}: redundant app binary')
        require(entry['board'] == board and entry['version'] == tag, f'{board}: metadata identity mismatch')
        # Exact project versions on tag builds prevent packaging stale artifacts
        # (including old EV-MUX firmware) under a newly qualified release tag.
        require(entry['firmwareVersion'] == tag, f'{board}: build version differs from tag')
        image, install = entry['image'], entry['install']
        require(image['evMuxVersion'] == 4, f'{board}: unsupported protocol')
        require(image['chipFamily'] in (0x3280, 0x3253, 0x3231), f'{board}: unsupported chip')
        require(0 <= image['chipRevisionMin'] <= install['chipRevisionMin']
                <= install['chipRevisionMax'] <= image['chipRevisionMax'], f'{board}: revision bounds mismatch')
        full, actual_full_name = image_bytes(directory, install)
        if 'source' in image:
            source = image['source']
            require(all(source[key] == install[key] for key in ('url', 'bytes', 'sha256'))
                    and image['url'] == source['url'] and source['offset'] == image['offset'],
                    f'{board}: source container differs from install image')
            start = source['offset']
            require(type(start) is int and start >= 0 and start + image['bytes'] <= len(full),
                    f'{board}: source slice out of bounds')
            application = full[start:start + image['bytes']]
            check_digest(application, image, f'{board}: source application')
            require(actual_full_name == full_name, f'{board}: config image mismatch')
        else:
            application, app_name = image_bytes(directory, image)
            require(actual_full_name == full_name and app_name != full_name, f'{board}: config image mismatch')
        require(install['offset'] == 0 and len(full) <= install['flashSize'], f'{board}: invalid flash bounds')
        boot, table = install['bootloader'], image['partitionTable']
        require(0 <= boot['offset'] < table['offset'] < image['offset']
                and boot['offset'] + boot['bytes'] <= table['offset']
                and table['offset'] + table['bytes'] <= image['offset']
                and image['offset'] + image['partitionSize'] <= install['flashSize']
                and len(application) <= image['partitionSize'], f'{board}: invalid layout')
        check_digest(full[boot['offset']:boot['offset'] + boot['bytes']], boot, f'{board}: bootloader')
        table_data = full[table['offset']:table['offset'] + table['bytes']]
        check_digest(table_data, table, f'{board}: partition table')
        check_digest(full[image['offset']:image['offset'] + image['bytes']], image, f'{board}: merged application')
        require(any(struct.unpack_from('<HBBII', table_data, cursor)[0:2] == (0x50AA, 0)
                    and struct.unpack_from('<HBBII', table_data, cursor)[3:5] == (image['offset'], image['partitionSize'])
                    for cursor in range(0, len(table_data) - 31, 32)), f'{board}: app partition is not declared')
    return len(boards)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag')
    parser.add_argument('--directory', default='binaries')
    parser.add_argument('--source-commit')
    parser.add_argument('--pipeline-id', type=int)
    parser.add_argument('--require-publication', action='store_true')
    args = parser.parse_args()
    # rtoml is already installed by pack_launchpad, including on Python <3.11.
    import rtoml
    config = rtoml.load(Path(args.directory) / 'config.toml')
    print(f'Validated {validate_release(args.directory, args.tag, config, args.source_commit, args.pipeline_id, args.require_publication)} qualified firmware releases')


if __name__ == '__main__':
    main()

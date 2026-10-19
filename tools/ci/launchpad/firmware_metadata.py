# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Publish qualified application updates and complete-install manifests."""
import hashlib
import json
from pathlib import Path
import struct


FAMILIES = {'esp32p4': 0x3280, 'esp32s3': 0x3253, 'esp32s31': 0x3231}


def publish_update(board, target, tag, build_dir, output_dir, full_image=None, layout="legacy"):
    from esptool.bin_image import LoadFirmwareImage
    build, output = Path(build_dir), Path(output_dir)
    args = json.loads((build / 'flasher_args.json').read_text())
    app, table = args['app'], args['partition-table']
    app_path = build / app['file']
    data = app_path.read_bytes()
    table_data = (build / table['file']).read_bytes()
    offset = int(app['offset'], 0)
    partition_size = None
    for cursor in range(0, len(table_data) - 31, 32):
        magic, kind, _subtype, start, size = struct.unpack_from('<HBBII', table_data, cursor)
        if magic == 0x50AA and kind == 0 and start == offset:
            partition_size = size
            break
    if partition_size is None or len(data) > partition_size:
        raise ValueError('Application does not fit a declared app partition')
    image = LoadFirmwareImage(target, str(app_path))
    name = f'esp-vision-{board}-{tag}-app.bin'
    if layout not in ('legacy', 'single-bin'):
        raise ValueError('Unknown firmware publication layout')
    if layout == 'legacy':
        (output / name).write_bytes(data)
    entry = {
        'board': board, 'version': tag,
        'firmwareVersion': json.loads((build / 'project_description.json').read_text())['project_version'],
        'image': {
            'evMuxVersion': 4, 'chipFamily': FAMILIES[target],
            'chipRevisionMin': image.min_rev_full,
            'chipRevisionMax': image.max_rev_full,
            'offset': offset, 'bytes': len(data), 'partitionSize': partition_size,
            'sha256': hashlib.sha256(data).hexdigest(),
            'url': 'https://dl.espressif.com/AE/esp-vision/bin/' + name,
            'partitionTable': {
                'offset': int(table['offset'], 0), 'bytes': len(table_data),
                'sha256': hashlib.sha256(table_data).hexdigest(),
            },
        },
    }
    if full_image is not None:
        full_path = Path(full_image)
        full = full_path.read_bytes()
        size_text = args['flash_settings']['flash_size']
        if not size_text.endswith('MB') or not size_text[:-2].isdigit():
            raise ValueError('Complete installation requires a fixed flash size in MB')
        flash_size = int(size_text[:-2]) * 1024 * 1024
        boot = args['bootloader']
        boot_data = (build / boot['file']).read_bytes()
        boot_offset = int(boot['offset'], 0)
        if not (0 <= boot_offset < int(table['offset'], 0) < offset
                and boot_offset + len(boot_data) <= int(table['offset'], 0)
                and int(table['offset'], 0) + len(table_data) <= offset
                and offset + len(data) <= len(full) <= flash_size):
            raise ValueError('Complete image bounds do not cover bootloader, partition table and application')
        # merge-bin can patch the bootloader flash mode/size/frequency header.
        # Hash the actual merged bootloader, not the pre-merge header.
        for part_offset, part_file in args['flash_files'].items():
            if int(part_offset, 0) == boot_offset:
                continue
            part = (build / part_file).read_bytes()
            start = int(part_offset, 0)
            if full[start:start + len(part)] != part:
                raise ValueError('Complete image differs from its declared flash inputs')
        boot_image = LoadFirmwareImage(target, str(build / boot['file']))
        minimum = max(image.min_rev_full, boot_image.min_rev_full)
        maximum = min(image.max_rev_full, boot_image.max_rev_full)
        if minimum > maximum:
            raise ValueError('Bootloader and application silicon revision ranges do not overlap')
        entry['install'] = {
            'offset': 0, 'bytes': len(full), 'flashSize': flash_size,
            'chipRevisionMin': minimum, 'chipRevisionMax': maximum,
            'sha256': hashlib.sha256(full).hexdigest(),
            'url': 'https://dl.espressif.com/AE/esp-vision/bin/' + full_path.name,
            'bootloader': {
                'offset': boot_offset, 'bytes': len(boot_data),
                'sha256': hashlib.sha256(full[boot_offset:boot_offset + len(boot_data)]).hexdigest(),
            },
        }
    identity_path = build / 'release-build-identity.json'
    if identity_path.is_file():
        identity = json.loads(identity_path.read_text())
        if (identity['board'] != board or identity['firmwareVersion'] != entry['firmwareVersion']
                or identity['appSha256'] != entry['image']['sha256']
                or type(identity['capabilities']['wifi']) is not bool):
            raise ValueError('Build identity does not match application/capabilities')
        entry['capabilities'] = identity['capabilities']
        entry['sourceCommit'] = identity['sourceCommit']
    if layout == 'single-bin':
        if full_image is None:
            raise ValueError('Single-bin publication requires the complete container')
        entry['image']['source'] = {key: entry['install'][key] for key in ('url', 'bytes', 'sha256')}
        entry['image']['source']['offset'] = entry['image']['offset']
        entry['image']['url'] = entry['install']['url']
    else:
        directory = output / 'firmware' / board
        directory.mkdir(parents=True, exist_ok=True)
        (directory / (tag + '.json')).write_text(json.dumps(entry, indent=2) + '\n')
    return entry

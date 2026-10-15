# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Publish qualified application updates beside existing full-flash binaries."""
import hashlib
import json
from pathlib import Path
import struct


FAMILIES = {'esp32p4': 0x3280, 'esp32s3': 0x3253, 'esp32s31': 0x3231}


def publish_update(board, target, tag, build_dir, output_dir):
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
    directory = output / 'firmware' / board
    directory.mkdir(parents=True, exist_ok=True)
    (directory / (tag + '.json')).write_text(json.dumps(entry, indent=2) + '\n')
    return entry

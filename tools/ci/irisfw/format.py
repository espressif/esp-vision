# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""ESP-VISION's unsigned application-package subset of Iris system-update 0.2.

This is a file-format implementation, independent of Iris device protocols.
The compatibility reference is esp-mosaico-utils at be151ae047014e79a9c7ae0092dd203844499003.
"""
import csv
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[3]
SCHEMA = 'esp-iris-system-update/0.2'
LIMIT = 32 * 1024 * 1024
COMPONENT_LIMIT = 16 * 1024 * 1024
BOARD = 'ESP32_S31_MOSAICO'
MINIMUM_RECOVERY_VERSION = '0.2.0'
PROFILE = 'mosaico-retained-irisfw'
# The fixed ABI 2 system prefix plus existing application NVS.
# Reference: esp-mosaico-utils product_contract.json at the commit above.
RETAINED_PARTITIONS = (
    (1, 0, 0x9000, 0x2000, 'otadata', 0),
    (1, 1, 0xb000, 0x1000, 'phy_init', 0),
    (1, 2, 0xc000, 0x14000, 'sysmeta', 0),
    (0, 0x20, 0x20000, 0x1c0000, 'vibe_mode', 0),
    (1, 3, 0x1e0000, 0x20000, 'coredump', 0),
    (1, 2, 0x200000, 0x10000, 'nvs', 0),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True) + '\n').encode()


def strict_json(data):
    if isinstance(data, bytes):
        data = data.decode('utf-8')
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, f'Duplicate JSON field: {key}')
            result[key] = value
        return result
    return json.loads(data, object_pairs_hook=pairs,
                      parse_constant=lambda value: require(False, f'Invalid JSON number: {value}'))


def fields(value, expected):
    require(isinstance(value, dict) and set(value) == set(expected), 'Missing or unknown fields')


def uint(value, maximum=0xffffffff):
    require(type(value) is int and 0 <= value <= maximum, 'Invalid unsigned integer')
    return value


def digest(value):
    require(isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value), 'Invalid SHA-256')
    return value


def filename(value):
    require(isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,127}', value),
            'Unsafe file name')
    return value


def release_tag(value):
    require(isinstance(value, str) and len(value) <= 31 and re.fullmatch(
        r'(?:v[0-9]+\.[0-9]+(?:\.[0-9]+)?|[0-9]{4}\.[0-9]{1,2}\.[0-9]{1,2})(?:[-.][A-Za-z0-9._+-]+)?', value),
        'Invalid firmware release tag (must fit the 31-byte application version)')
    return value


def source_sha(value):
    require(isinstance(value, str) and re.fullmatch('[0-9a-f]{40}', value), 'Expected full source SHA')
    return value


def read_input(root, name):
    require(isinstance(name, str) and name and not Path(name).is_absolute()
            and '..' not in Path(name).parts and '\\' not in name, 'Unsafe build input path')
    path = (Path(root) / name).resolve()
    require(path.is_relative_to(Path(root).resolve()) and path.is_file(), 'Missing or escaping build input')
    require(path.stat().st_size <= COMPONENT_LIMIT, 'Build input exceeds size limit')
    return path.read_bytes()


def settings(text):
    return dict(re.findall(r'^(CONFIG_[A-Z0-9_]+)=(.*)$', text, re.MULTILINE))


def board_policy(board, root=ROOT):
    """Derive board, flash and layout from the existing release catalog/source."""
    import yaml
    catalog = yaml.safe_load((root / 'tools/ci/launchpad/boards.yml').read_text())['boards']
    require(board == BOARD and board in catalog, 'Iris firmware is supported only for ESP32_S31_MOSAICO')
    target = catalog[board]['target']
    require(target == 'esp32s31', 'Iris board target mismatch')
    port = root / 'boards' / board / 'port'
    config = settings((port / 'sdkconfig.board').read_text())
    config.update(settings((port / 'sdkconfig.irisfw').read_text()))
    sizes = [int(key.removeprefix('CONFIG_ESPTOOLPY_FLASHSIZE_').removesuffix('MB')) * 1024 * 1024
             for key, value in config.items() if re.fullmatch(r'CONFIG_ESPTOOLPY_FLASHSIZE_[0-9]+MB', key) and value == 'y']
    require(len(sizes) == 1, 'Board must declare one fixed Flash size')
    csv_name = Path(json.loads(config['CONFIG_PARTITION_TABLE_CUSTOM_FILENAME'])).name
    partitions = []
    types = {'app': 0, 'data': 1}
    subtypes = {'ota': 0, 'nvs': 2, 'phy': 1, 'fat': 0x81, 'coredump': 3,
                'test': 0x20, 'ota_0': 0x10, '0x40': 0x40}
    for row in csv.reader((port / csv_name).read_text().splitlines()):
        if not row or row[0].lstrip().startswith('#'):
            continue
        row = [value.strip() for value in row]
        require(len(row) >= 6 and not row[5], 'Unsupported partition flags')
        label, kind, subtype, offset, size = row[:5]
        require(kind in types and subtype in subtypes, 'Unsupported release partition policy')
        partitions.append((types[kind], subtypes[subtype], int(offset, 0), int(size, 0), label, 0))
    require(all(entry in partitions for entry in RETAINED_PARTITIONS), 'Retained Vibe Mode layout changed')
    require((0, 0x10, 0x210000, 0x700000, 'main_app', 0) in partitions
            and (1, 0x81, 0x910000, sizes[0] - 0x910000, 'ffat', 0) in partitions
            and len(partitions) == 8 and sizes[0] == 16 * 1024 * 1024, 'Iris application/storage layout mismatch')
    return dict(board=board, target=target, chip_id=32, flash_size=sizes[0],
                build_directory='idf6.1-irisfw', idf_image='release-v6.1', partitions=partitions)


def release_boards(root=ROOT):
    import yaml
    catalog = yaml.safe_load((root / 'tools/ci/launchpad/boards.yml').read_text())['boards']
    require(BOARD in catalog, 'Iris board is missing from the release catalog')
    return [BOARD]


def application_partition(policy):
    slots = [p for p in policy['partitions'] if p[:2] == (0, 0x10) and p[4] == 'main_app']
    require(len(slots) == 1 and slots[0][2] == 0x210000, 'Missing retained main_app partition')
    return slots[0]


def partition_table(data, policy, offset=0x8000):
    require(len(data) in (0xc00, 0x1000), 'Partition table must be 3072 or 4096 bytes')
    require(offset == 0x8000, 'Unsupported partition-table address')
    entries = []
    previous_end = offset + 4096
    for cursor in range(0, len(data), 32):
        entry = data[cursor:cursor + 32]
        magic = struct.unpack_from('<H', entry)[0]
        if magic == 0xebeb:
            require(entry[2:16] == b'\xff' * 14 and entry[16:] == hashlib.md5(data[:cursor]).digest(),
                    'Partition table MD5 mismatch')
            require(data[cursor + 32:] == b'\xff' * (len(data) - cursor - 32), 'Partition table trailing data')
            break
        require(magic == 0x50aa, 'Partition table missing entries or MD5')
        _, kind, subtype, start, size, raw_label, flags = struct.unpack('<HBBII16sI', entry)
        label = raw_label.split(b'\0', 1)[0].decode('ascii')
        require(size > 0 and start >= previous_end and start % (65536 if kind == 0 else 4096) == 0
                and start + size <= policy['flash_size'], 'Partition overlap, alignment or Flash bounds')
        previous_end = start + size
        entries.append((kind, subtype, start, size, label, flags))
    else:
        raise ValueError('Partition table MD5 is missing')
    require(entries == policy['partitions'], 'Partition layout differs from board policy')
    return data.ljust(4096, b'\xff')


def inspect_application(data, chip_id):
    """Validate segment bounds, ESP XOR checksum, appended hash and app identity."""
    require(288 <= len(data) <= COMPONENT_LIMIT and data[0] == 0xe9, 'Invalid ESP application')
    require(struct.unpack_from('<H', data, 12)[0] == chip_id, 'Application chip ID mismatch')
    count = data[1]
    require(1 <= count <= 16 and data[23] in (0, 1), 'Invalid ESP image header')
    cursor, checksum = 24, 0xef
    for index in range(count):
        require(cursor + 8 <= len(data), 'Truncated segment header')
        size = struct.unpack_from('<I', data, cursor + 4)[0]
        cursor += 8
        require(size <= len(data) - cursor, 'Truncated image segment')
        if index == 0:
            require(size >= 256 and struct.unpack_from('<I', data, cursor)[0] == 0xabcd5432,
                    'Application descriptor missing')
        for byte in data[cursor:cursor + size]:
            checksum ^= byte
        cursor += size
    checksum_position = (cursor // 16 + 1) * 16 - 1
    require(checksum_position < len(data) and data[checksum_position] == checksum, 'ESP checksum mismatch')
    end = checksum_position + 1
    if data[23]:
        require(data[end:end + 32] == hashlib.sha256(data[:end]).digest(), 'ESP image SHA-256 mismatch')
        end += 32
    require(end == len(data), 'Unsupported trailing image data (signed images need a separate policy)')

    def cstring(offset, size):
        raw = data[offset:offset + size]
        require(b'\0' in raw, 'Unterminated application descriptor string')
        value = raw.split(b'\0', 1)[0].decode('utf-8')
        require(value and value.isprintable(), 'Empty or invalid application identity')
        return value
    return dict(version=cstring(48, 32), project=cstring(80, 32),
                idf_version=cstring(144, 32), elf_sha256=data[176:208].hex())

def validate_build_record(record, policy, tag, source_commit, pipeline_id):
    fields(record, ('schema', 'profile', 'board', 'target', 'source_commit', 'pipeline_id', 'job_id',
                    'idf_image', 'idf_commit', 'application', 'flash_size', 'sdkconfig_sha256',
                    'inputs', 'layout_sha256'))
    require(record['schema'] == 1 and record['profile'] == PROFILE
            and record['board'] == policy['board'] and record['target'] == policy['target']
            and source_sha(record['source_commit']) == source_commit and uint(record['pipeline_id']) == pipeline_id
            and uint(record['job_id']) > 0, 'Build source/board/pipeline mismatch')
    require(record['idf_image'] == policy['idf_image'] and uint(record['flash_size']) == policy['flash_size'],
            'Build IDF selection or Flash size mismatch')
    source_sha(record['idf_commit'])
    digest(record['sdkconfig_sha256'])
    digest(record['layout_sha256'])
    fields(record['application'], ('version', 'project', 'idf_version', 'elf_sha256'))
    app = record['application']
    require(app['version'] == tag and app['project'] == 'esp-vision', 'Build version/project mismatch')
    require(isinstance(app['idf_version'], str) and app['idf_version'].startswith(
        'v6.1' if policy['target'] == 'esp32s31' else 'v5.5.6'), 'Unexpected compiled IDF version')
    digest(app['elf_sha256'])
    fields(record['inputs'], ('application', 'partition_table', 'bootloader', 'ota_data', 'flasher_args', 'project_description'))
    for value in record['inputs'].values():
        fields(value, ('file', 'sha256', 'size'))
        require(isinstance(value['file'], str) and value['file'], 'Invalid build input name')
        digest(value['sha256'])
        require(uint(value['size']) > 0, 'Empty build input')

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Release gate regression tests; no IDF build or network required."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('validate_release', Path(__file__).resolve().parents[1] / 'ci/launchpad/validate_release.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def descriptor(data, **fields):
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), **fields)


class FirmwareReleasePackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.app = b'qualified application'
        table = struct.pack('<HBBII16sI', 0x50AA, 0, 0, 65536, 4096, b'factory', 0)
        full = bytearray(b'\xff' * (65536 + len(self.app)))
        full[8192:8196] = b'boot'
        full[32768:32768 + len(table)] = table
        full[65536:] = self.app
        self.entry = {
            'board': 'BOARD', 'version': 'v1', 'firmwareVersion': 'v1',
            'image': descriptor(self.app, url=MODULE.ROOT_URL + 'bin/app.bin', offset=65536, partitionSize=4096,
                                chipFamily=0x3280, evMuxVersion=4, chipRevisionMin=301, chipRevisionMax=399,
                                partitionTable=descriptor(table, offset=32768)),
            'install': descriptor(full, url=MODULE.ROOT_URL + 'bin/full.bin', offset=0, flashSize=16 * 1024 * 1024,
                                  chipRevisionMin=301, chipRevisionMax=399, bootloader=descriptor(b'boot', offset=8192)),
        }
        self.config = {'firmware_images_url': MODULE.ROOT_URL + 'bin/', 'website': {'release': 'v1', 'releases': ['v1', 'legacy']},
                       'supported_apps': ['esp-vision'], 'esp-vision': {'image': {'BOARD': 'full.bin'}}}
        (self.root / 'app.bin').write_bytes(self.app)
        (self.root / 'full.bin').write_bytes(full)
        self.path = self.root / 'firmware/BOARD/v1.json'
        self.path.parent.mkdir(parents=True)
        self.save()

    def save(self):
        self.path.write_text(json.dumps(self.entry))

    def validate(self):
        self.save()
        return MODULE.validate_release(self.root, 'v1', self.config)

    def test_complete_package_and_unqualified_history(self):
        self.assertEqual(self.validate(), 1)
        self.assertFalse((self.root / 'firmware/BOARD/legacy.json').exists())

    def test_missing_or_bin_nested_metadata_is_rejected(self):
        nested = self.root / 'bin/firmware/BOARD/v1.json'
        nested.parent.mkdir(parents=True)
        self.path.rename(nested)
        with self.assertRaises(FileNotFoundError):
            MODULE.validate_release(self.root, 'v1', self.config)

    def test_stale_build_and_wrong_metadata_identity_are_rejected(self):
        for key, value in [('firmwareVersion', 'legacy'), ('version', 'legacy'), ('board', 'OTHER')]:
            with self.subTest(key=key):
                entry = copy.deepcopy(self.entry)
                self.entry[key] = value
                with self.assertRaises(ValueError):
                    self.validate()
                self.entry = entry

    def test_app_only_metadata_cannot_pass_complete_install_release_gate(self):
        del self.entry['install']
        with self.assertRaises(KeyError):
            self.validate()

    def test_corrupt_application_and_full_image_are_rejected(self):
        for name in ('app.bin', 'full.bin'):
            with self.subTest(name=name):
                path = self.root / name
                original = path.read_bytes()
                path.write_bytes(original + b'corrupt')
                with self.assertRaisesRegex(ValueError, 'size mismatch'):
                    self.validate()
                path.write_bytes(original)

    def test_config_image_and_revision_mismatch_are_rejected(self):
        self.config['esp-vision']['image']['BOARD'] = 'other.bin'
        with self.assertRaisesRegex(ValueError, 'config image'):
            self.validate()
        self.config['esp-vision']['image']['BOARD'] = 'full.bin'
        self.entry['install']['chipRevisionMin'] = 1
        with self.assertRaisesRegex(ValueError, 'revision'):
            self.validate()

    def test_partition_and_bootloader_hashes_are_checked(self):
        for part in (self.entry['image']['partitionTable'], self.entry['install']['bootloader']):
            original = part['sha256']
            part['sha256'] = '0' * 64
            with self.assertRaisesRegex(ValueError, 'SHA-256'):
                self.validate()
            part['sha256'] = original

    def test_out_of_bounds_layout_is_rejected(self):
        self.entry['image']['partitionSize'] = self.entry['install']['flashSize']
        with self.assertRaisesRegex(ValueError, 'invalid layout'):
            self.validate()

    def test_unexpected_image_host_and_path_traversal_are_rejected(self):
        for url in ('https://other.example/app.bin', MODULE.ROOT_URL + 'bin/../app.bin'):
            self.entry['image']['url'] = url
            with self.assertRaises(ValueError):
                self.validate()

    def test_sdk_tag_and_mismatched_catalog_cannot_be_published(self):
        with self.assertRaisesRegex(ValueError, 'SDK tags'):
            MODULE.validate_release(self.root, 'device-core-v0.3.0', self.config)
        self.config['website']['release'] = 'legacy'
        with self.assertRaisesRegex(ValueError, 'pipeline tag'):
            self.validate()

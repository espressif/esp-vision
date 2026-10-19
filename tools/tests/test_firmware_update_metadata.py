# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('firmware_metadata', ROOT / 'tools/ci/launchpad/firmware_metadata.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
VALIDATOR_SPEC = importlib.util.spec_from_file_location('validate_release', ROOT / 'tools/ci/launchpad/validate_release.py')
VALIDATOR = importlib.util.module_from_spec(VALIDATOR_SPEC)
VALIDATOR_SPEC.loader.exec_module(VALIDATOR)


class FirmwareMetadataTests(unittest.TestCase):
    def test_release_publishes_app_only_with_real_partition_bounds_and_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'app.bin').write_bytes(b'application')
            table = struct.pack('<HBBII16sI', 0x50AA, 0, 0, 65536, 4096, b'factory', 0)
            (root / 'table.bin').write_bytes(table)
            (root / 'flasher_args.json').write_text(json.dumps({'app': {'offset': '0x10000', 'file': 'app.bin'}, 'partition-table': {'offset': '0x8000', 'file': 'table.bin'}}))
            (root / 'project_description.json').write_text('{"project_version":"v1"}')
            fake = SimpleNamespace(LoadFirmwareImage=lambda *_: SimpleNamespace(min_rev_full=301, max_rev_full=399))
            with patch.dict(sys.modules, {'esptool': SimpleNamespace(), 'esptool.bin_image': fake}):
                result = MODULE.publish_update('BOARD', 'esp32p4', 'v1', root, root)
            self.assertEqual(result['image']['sha256'], hashlib.sha256(b'application').hexdigest())
            self.assertEqual(result['image']['partitionSize'], 4096)
            self.assertEqual(result['image']['chipRevisionMin'], 301)
            self.assertEqual((root / 'esp-vision-BOARD-v1-app.bin').read_bytes(), b'application')
            self.assertTrue((root / 'firmware/BOARD/v1.json').exists())
            self.assertEqual((root / 'table.bin').read_bytes(), table)

    def test_oversized_app_is_never_published_as_a_safe_update(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'app.bin').write_bytes(b'oversized')
            (root / 'table.bin').write_bytes(struct.pack('<HBBII16sI', 0x50AA, 0, 0, 65536, 1, b'factory', 0))
            (root / 'flasher_args.json').write_text(json.dumps({'app': {'offset': '0x10000', 'file': 'app.bin'}, 'partition-table': {'offset': '0x8000', 'file': 'table.bin'}}))
            with patch.dict(sys.modules, {'esptool': SimpleNamespace(), 'esptool.bin_image': SimpleNamespace(LoadFirmwareImage=lambda *_: None)}):
                with self.assertRaisesRegex(ValueError, 'fit'):
                    MODULE.publish_update('BOARD', 'esp32p4', 'v1', root, root)

    def test_complete_install_manifest_uses_merged_bootloader_and_validates_all_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = b'application'
            table = struct.pack('<HBBII16sI', 0x50AA, 0, 0, 65536, 4096, b'factory', 0)
            boot = b'bootloader'
            for name, data in [('app.bin', app), ('table.bin', table), ('boot.bin', boot)]:
                (root / name).write_bytes(data)
            args = {'app': {'offset': '0x10000', 'file': 'app.bin'},
                    'bootloader': {'offset': '0x2000', 'file': 'boot.bin'},
                    'partition-table': {'offset': '0x8000', 'file': 'table.bin'},
                    'flash_settings': {'flash_size': '16MB'},
                    'flash_files': {'0x2000': 'boot.bin', '0x8000': 'table.bin', '0x10000': 'app.bin'}}
            (root / 'flasher_args.json').write_text(json.dumps(args))
            (root / 'project_description.json').write_text('{"project_version":"v1"}')
            full = bytearray(b'\xff' * (65536 + len(app)))
            full[8192:8192 + len(boot)] = boot
            full[8192] = 0xe9  # merge-bin may rewrite the bootloader header.
            full[32768:32768 + len(table)] = table
            full[65536:] = app
            full_path = root / 'full.bin'
            full_path.write_bytes(full)
            fake = SimpleNamespace(LoadFirmwareImage=lambda *_: SimpleNamespace(min_rev_full=301, max_rev_full=399))
            with patch.dict(sys.modules, {'esptool': SimpleNamespace(), 'esptool.bin_image': fake}):
                entry = MODULE.publish_update('BOARD', 'esp32p4', 'v1', root, root, full_path)
                self.assertEqual(entry['install']['offset'], 0)
                self.assertEqual(entry['install']['flashSize'], 16 * 1024 * 1024)
                self.assertEqual(entry['install']['sha256'], hashlib.sha256(full).hexdigest())
                self.assertEqual(entry['install']['bootloader']['sha256'], hashlib.sha256(full[8192:8192 + len(boot)]).hexdigest())
                self.assertEqual(entry['image']['sha256'], hashlib.sha256(app).hexdigest())
                config = {'website': {'release': 'v1'}, 'firmware_images_url': VALIDATOR.ROOT_URL + 'bin/',
                          'supported_apps': ['esp-vision'], 'esp-vision': {'image': {'BOARD': 'full.bin'}}}
                self.assertEqual(VALIDATOR.validate_release(root, 'v1', config), 1)
                identity = {'board': 'BOARD', 'firmwareVersion': 'v1', 'sourceCommit': 'a' * 40,
                            'appSha256': hashlib.sha256(app).hexdigest(), 'capabilities': {'wifi': False}}
                (root / 'release-build-identity.json').write_text(json.dumps(identity))
                single = root / 'single'
                single.mkdir()
                single_entry = MODULE.publish_update('BOARD', 'esp32p4', 'v1', root, single,
                                                     full_path, layout='single-bin')
                self.assertFalse((single / 'esp-vision-BOARD-v1-app.bin').exists())
                self.assertFalse((single / 'firmware/BOARD/v1.json').exists())
                self.assertEqual(single_entry['capabilities'], {'wifi': False})
                self.assertEqual(single_entry['sourceCommit'], 'a' * 40)
                self.assertEqual(single_entry['image']['source']['sha256'], entry['install']['sha256'])
                self.assertEqual(single_entry['image']['sha256'], entry['image']['sha256'])
                identity['appSha256'] = '0' * 64
                (root / 'release-build-identity.json').write_text(json.dumps(identity))
                with self.assertRaisesRegex(ValueError, 'Build identity'):
                    MODULE.publish_update('BOARD', 'esp32p4', 'v1', root, single, full_path, layout='single-bin')
                (root / 'release-build-identity.json').unlink()
                full[65536] ^= 1
                full_path.write_bytes(full)
                with self.assertRaisesRegex(ValueError, 'flash inputs'):
                    MODULE.publish_update('BOARD', 'esp32p4', 'v1', root, root, full_path)

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

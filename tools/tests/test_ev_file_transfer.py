# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Run the actual native binary upload engine against a FAT-like POSIX test volume."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class FileTransferTests(unittest.TestCase):
    def test_native_engine_and_failure_lifecycles(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler unavailable')
        with tempfile.TemporaryDirectory(prefix='ev-file-transfer-') as directory:
            out = Path(directory)
            volume = out / 'flash'
            volume.mkdir()
            # Copy the implementation unchanged so the test HAL shadows IDF headers.
            source = out / 'ev_file_transfer.c'
            shutil.copyfile(ROOT / 'platform/ev_file_transfer.c', source)
            options = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-I' + str(ROOT / 'tools/tests/ev_file_transfer_stubs'),
                       '-I' + str(ROOT / 'platform'), '-pthread']
            subprocess.run([*options, f'-DESP_VISION_STORAGE_FLASH_PATH="{volume}"',
                            '-Dwrite=ev_test_write', '-Dclose=ev_test_close',
                            '-Dfsync=ev_test_fsync', '-Drename=ev_test_rename',
                            '-c', str(source), '-o', str(out / 'engine.o')], check=True)
            subprocess.run([*options, str(ROOT / 'tools/tests/ev_file_transfer_test.c'),
                            str(out / 'engine.o'), '-lcrypto', '-o', str(out / 'test')], check=True)
            subprocess.run([str(out / 'test'), str(volume)], check=True)

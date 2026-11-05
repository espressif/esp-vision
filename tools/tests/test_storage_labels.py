# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise default naming without formatting, rewriting user labels or bypassing ownership."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class StorageLabelTests(unittest.TestCase):
    def test_default_labels_preservation_and_failures(self):
        compiler = shutil.which('cc')
        if compiler is None:
            self.skipTest('a native C compiler is unavailable')
        with tempfile.TemporaryDirectory(prefix='ev-storage-label-') as temporary:
            executable = Path(temporary) / 'labels'
            subprocess.run([
                compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                f'-I{ROOT / "tools/tests/storage_writer_service_stubs"}',
                f'-I{ROOT / "tools/tests/storage_provision_stubs"}', f'-I{ROOT / "platform"}',
                str(ROOT / 'platform/storage_label.c'),
                str(ROOT / 'tools/tests/storage_label_host_test.c'), '-o', str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)

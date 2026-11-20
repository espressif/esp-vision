# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Fault-inject the production TinyUSB video completion handler."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class TinyUsbVideoTests(unittest.TestCase):
    def test_transfer_completion_and_error_boundaries(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler unavailable')
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / 'video-transfer-test'
            subprocess.run(
                [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                 f'-I{ROOT / "overlay/micropython/shared/tinyusb"}',
                 str(Path(__file__).with_name('tinyusb_video_transfer_host_test.c')),
                 '-o', str(executable)],
                check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()

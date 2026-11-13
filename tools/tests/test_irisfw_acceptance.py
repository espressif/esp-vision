# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real installer compatibility code without enabling EV-MUX output."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class IrisAcceptanceTests(unittest.TestCase):
    def test_native_installer_session_and_isolation(self):
        compiler = shutil.which('cc') or shutil.which('gcc')
        self.assertIsNotNone(compiler, 'Native C compiler required')
        with tempfile.TemporaryDirectory(prefix='irisfw-acceptance-') as temporary:
            binary = Path(temporary) / 'acceptance-test'
            includes = ('tools/tests/firmware_status_stubs', 'tools/tests/irisfw_network_stubs',
                        'platform', 'boards/ESP32_S31_MOSAICO')
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-DESP_VISION_ENABLE_IRISFW=1',
                            '-DESP_VISION_FIRMWARE_VERSION="test-firmware"',
                            *['-I' + str(ROOT / path) for path in includes],
                            str(ROOT / 'tools/tests/irisfw_acceptance_host_test.c'),
                            '-o', str(binary)], check=True)
            for scenario in ('ready', 'foreign-owner', 'framing', 'not-ready', 'crash', 'tx-retry'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True)


if __name__ == '__main__':
    unittest.main()

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Run the production Mosaico shared-NVS backend against bounded NVS fakes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class IrisNetworkTests(unittest.TestCase):
    def test_shared_credentials_and_storage_failures(self):
        compiler = shutil.which('cc') or shutil.which('gcc')
        self.assertIsNotNone(compiler, 'Native C compiler required')
        with tempfile.TemporaryDirectory(prefix='irisfw-network-') as temporary:
            binary = Path(temporary) / 'network-test'
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'tools/tests/irisfw_network_stubs'),
                            '-I' + str(ROOT / 'boards/ESP32_S31_MOSAICO'),
                            str(ROOT / 'boards/ESP32_S31_MOSAICO/irisfw_network.c'),
                            str(ROOT / 'tools/tests/irisfw_network_host_test.c'), '-o', str(binary)], check=True)
            for scenario in ('legacy', 'absent', 'shared', 'partial-pair', 'bounds',
                             'password-format', 'persist', 'forget', 'metadata', 'errors'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True)


if __name__ == '__main__':
    unittest.main()

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise production installation evidence with native SHA-256 and fault injection."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class FirmwareStatusTests(unittest.TestCase):
    def test_read_only_evidence_and_confirmation_failures(self):
        compiler = shutil.which('cc') or shutil.which('gcc')
        self.assertIsNotNone(compiler, 'Native C compiler required')
        with tempfile.TemporaryDirectory(prefix='firmware-status-') as temporary:
            binary = Path(temporary) / 'status-test'
            includes = ('tools/tests/firmware_status_stubs', 'tools/tests/irisfw_network_stubs',
                        'tools/tests/ev_file_transfer_stubs', 'platform', 'boards/ESP32_S31_MOSAICO')
            sources = ('boards/ESP32_S31_MOSAICO/irisfw_boot.c',
                       'boards/ESP32_S31_MOSAICO/irisfw_status.c', 'tools/tests/firmware_status_host_test.c')
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            *['-I' + str(ROOT / item) for item in includes],
                            *[str(ROOT / item) for item in sources], '-lcrypto', '-o', str(binary)], check=True)
            for scenario in ('success', 'missing-app', 'bad-address', 'bad-capacity', 'bad-ota-state',
                             'ota-read-error', 'ota-confirm-error', 'nvs-init-error', 'nvs-write-error',
                             'nvs-commit-error', 'receipt-missing', 'receipt-magic', 'receipt-version',
                             'receipt-failure', 'receipt-size', 'receipt-read-error',
                             'image-read-error', 'table-read-error', 'bootloader-read-error', 'crash'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True)


if __name__ == '__main__':
    unittest.main()

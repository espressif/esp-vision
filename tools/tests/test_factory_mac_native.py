# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile the four production factory-ID paths against 6/8-byte MAC backends."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def extract_function(path, name):
    source = path.read_text()
    match = re.search(r'(?:static\s+)?(?:mp_obj_t|void)\s+' + name + r'\([^)]*\)\s*\{', source)
    if match is None:
        raise AssertionError('Production function missing: ' + name)
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    # Preserve the actual upstream MIT header on every extracted function.
    return source[:source.index('*/') + 2] + '\n' + source[match.start():end] + '\n'


class FactoryMacNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if compiler is None:
            raise unittest.SkipTest('a native C compiler is unavailable')
        cls.temporary = tempfile.TemporaryDirectory(prefix='factory-mac-native-')
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        (directory / 'esp_mac.h').write_text('''#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_MAC_BASE, ESP_MAC_EFUSE_FACTORY } esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
esp_err_t esp_efuse_mac_get_default(uint8_t *mac);
''')
        (directory / 'boardconfig.h').write_text('#define ESP_VISION_BOARD_TYPE "ESP32_FACTORY_MAC_TEST"\n')
        functions = [
            ('overlay/micropython/ports/esp32/modmachine.c', 'mp_machine_unique_id'),
            ('overlay/micropython/ports/esp32/usb.c', 'mp_usbd_port_get_serial_number'),
        ]
        (directory / 'factory_mac_callers.inc').write_text(''.join(
            extract_function(ROOT / path, name) for path, name in functions))
        cls.binaries = []
        for ieee802154 in (0, 1):
            binary = directory / f'factory-mac-{ieee802154}'
            subprocess.run([
                compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                f'-DTEST_IEEE802154={ieee802154}',
                '-DESP_VISION_FIRMWARE_VERSION="factory-mac-test"',
                f'-I{directory}',
                f'-I{ROOT / "tools/tests/ev_launcher_state_stubs"}',
                f'-I{ROOT / "platform"}',
                str(ROOT / 'tools/tests/factory_mac_host_test.c'),
                str(ROOT / 'platform/device_identity.c'),
                str(ROOT / 'platform/ev_launcher_state.c'),
                '-o', str(binary),
            ], check=True, cwd=ROOT)
            cls.binaries.append(binary)

    def run_case(self, case):
        for binary in self.binaries:
            with self.subTest(ieee802154=binary.name[-1]):
                subprocess.run([str(binary), case], check=True, cwd=ROOT)

    def test_legacy_eui64_write_and_factory_eui48_boundary_canaries(self):
        self.run_case('contract-canaries')

    def test_production_machine_unique_id(self):
        self.run_case('machine')

    def test_production_usb_serial_and_output_canaries(self):
        self.run_case('usb')

    def test_production_canonical_device_identity(self):
        self.run_case('identity')

    def test_production_launcher_identity(self):
        self.run_case('launcher')

    def test_production_launcher_rejects_failed_factory_read(self):
        self.run_case('launcher-error')

    def test_production_read_failures_do_not_expose_uninitialized_identity(self):
        self.run_case('read-errors')


if __name__ == '__main__':
    unittest.main()

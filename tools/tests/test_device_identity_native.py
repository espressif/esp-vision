# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile canonical device identity with representative real build inputs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DeviceIdentityTests(unittest.TestCase):
    def test_dns_identity_and_long_build_versions(self):
        compiler = shutil.which('cc')
        if compiler is None:
            self.skipTest('C compiler unavailable')
        for version, expected in [('1.2.3', '1-2-3'), ('v0.9.0-rc.1+build', 'v0-9-0-rc-1-build'), ('a' * 100, None)]:
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / 'boardconfig.h').write_text('#define ESP_VISION_BOARD_TYPE "ESP32_P4X_FUNCTION_EV_BOARD"\n')
                (root / 'esp_mac.h').write_text('''#include <stdint.h>
#define ESP_OK 0
int esp_efuse_mac_get_default(uint8_t *mac);
''')
                (root / 'test.c').write_text('''#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "device_identity.h"
int esp_efuse_mac_get_default(uint8_t *mac) {
    const uint8_t id[] = {0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56};
    memcpy(mac, id, 6); return 0;
}
int main(int argc, char **argv) {
    assert(esp_vision_identity_init());
    assert(!strcmp(esp_vision_device_id(), "aabbcc123456"));
    const char *name = esp_vision_hostname();
    const char *prefix = "esp-vision-esp32-p4x-function-ev-board-123456-";
    assert(!strncmp(name, prefix, strlen(prefix)));
    assert(strlen(name) <= 63);
    assert(name[0] != '-' && name[strlen(name) - 1] != '-');
    for (const char *p = name; *p; ++p) assert(islower(*p) || isdigit(*p) || *p == '-');
    if (argc > 1) assert(!strcmp(name + strlen(prefix), argv[1]));
    char original[64]; strcpy(original, name);
    assert(esp_vision_identity_init()); assert(!strcmp(original, esp_vision_hostname()));
    return 0;
}
''')
                binary = root / 'test'
                subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{root}', f'-I{ROOT / "platform"}', f'-DESP_VISION_FIRMWARE_VERSION="{version}"', str(ROOT / 'platform/device_identity.c'), str(root / 'test.c'), '-o', str(binary)], check=True)
                subprocess.run([str(binary), *([expected] if expected else [])], check=True)


if __name__ == '__main__':
    unittest.main()

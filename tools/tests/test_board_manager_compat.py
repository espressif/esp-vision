# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Compile generated SPI settings across the SDK backport boundaries."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("board_manager_compat", ROOT / "tools/board_manager_compat.py")
compat = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compat)

GENERATED = '''static const dev_display_lcd_config_t esp_bmgr_display_lcd_cfg = {
    .name = "display_lcd",
    .sub_type = "spi",
    .sub_cfg = {
        .spi = {
            .io_spi_config = {
                .pclk_hz = 80000000,
                .flags = {
                    .sio_mode = false,
                    .lsb_first = false,
                },
            },
        },
    },
};
'''


class BoardManagerCompatTests(unittest.TestCase):
    def test_backport_boundaries_generate_c_compatible_with_old_and_new_headers(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("native C compiler unavailable")
        cases = [((5, 4, 99), False), ((5, 5, 0), False), ((5, 5, 4), False), ((5, 5, 5), True),
                 ((5, 5, 6), True), ((5, 5, 99), True), ((5, 6, 0), False), ((6, 0, 0), False),
                 ((6, 0, 1), False), ((6, 0, 2), True), ((6, 0, 3), True), ((6, 1, 0), True), ((6, 2, 0), True)]
        with tempfile.TemporaryDirectory(prefix="board-manager-compat-") as directory:
            source = Path(directory) / "settings.c"
            binary = Path(directory) / "settings"
            for version, supported in cases:
                with self.subTest(version=version):
                    patched, changes = compat.patch_generated_spi(GENERATED, {"display_lcd": True}, version)
                    self.assertEqual(bool(changes), supported)
                    flag = "bool psram_dma_direct;" if supported else ""
                    check = "assert(esp_bmgr_display_lcd_cfg.sub_cfg.spi.io_spi_config.flags.psram_dma_direct);" if supported else ""
                    source.write_text(f'''#include <stdbool.h>
#include <assert.h>
typedef struct {{ const char *name, *sub_type; struct {{ struct {{ struct {{
    unsigned pclk_hz; struct {{ bool sio_mode, lsb_first; {flag} }} flags;
}} io_spi_config; }} spi; }} sub_cfg; }} dev_display_lcd_config_t;
{patched}
int main(void) {{ assert(esp_bmgr_display_lcd_cfg.sub_cfg.spi.io_spi_config.pclk_hz == 80000000); {check} return 0; }}
''')
                    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True, capture_output=True)
                    subprocess.run([str(binary)], check=True)

    def test_explicit_false_and_existing_field_are_respected_and_idempotent(self):
        existing = GENERATED.replace(".sio_mode = false,", ".sio_mode = false,\n                    .psram_dma_direct = true,")
        patched, changes = compat.patch_generated_spi(existing, {"display_lcd": False}, (5, 5, 6))
        self.assertEqual(changes, ["display_lcd"])
        self.assertEqual(patched.count(".psram_dma_direct"), 1)
        self.assertIn(".psram_dma_direct = false,", patched)
        self.assertEqual(compat.patch_generated_spi(patched, {"display_lcd": False}, (5, 5, 6)), (patched, []))
        self.assertEqual(compat.patch_generated_spi(existing, {}, (5, 5, 6)), (existing, []))

    def test_only_selected_spi_device_is_changed(self):
        other = GENERATED.replace("esp_bmgr_display_lcd_cfg", "esp_bmgr_other_cfg").replace('"display_lcd"', '"other"')
        rgb = GENERATED.replace("esp_bmgr_display_lcd_cfg", "esp_bmgr_rgb_cfg").replace('"display_lcd"', '"rgb"').replace('"spi"', '"rgb"')
        patched, changes = compat.patch_generated_spi(other + rgb + GENERATED, {"display_lcd": True}, (6, 0, 2))
        self.assertTrue(patched.startswith(other + rgb))
        self.assertEqual(changes, ["display_lcd"])
        self.assertEqual(patched.count(".psram_dma_direct"), 1)

    def test_layout_changes_and_missing_device_fail_instead_of_silent_fallback(self):
        with self.assertRaisesRegex(ValueError, "recognizable IO flags"):
            compat.patch_generated_spi(GENERATED.replace(".io_spi_config", ".renamed"), {"display_lcd": True}, (5, 5, 6))
        with self.assertRaisesRegex(ValueError, "absent"):
            compat.patch_generated_spi(GENERATED, {"unknown": True}, (5, 5, 6))
        with self.assertRaisesRegex(ValueError, "Duplicate generated SPI LCD"):
            compat.patch_generated_spi(GENERATED * 2, {"display_lcd": True}, (6, 1, 0))

    def test_board_yaml_true_false_amendment_and_absent_flag(self):
        with tempfile.TemporaryDirectory(prefix="board-manager-input-") as directory:
            base = Path(directory) / "base"
            amend = Path(directory) / "amend"
            base.mkdir()
            amend.mkdir()
            text = "devices:\n  - name: display_lcd\n    config:\n      io_spi_config:\n        flags:\n          psram_dma_direct: true\n"
            (base / "board_devices.yaml").write_text(text)
            (amend / "board_devices.yaml").write_text(text.replace("true", "false"))
            self.assertEqual(compat.requested_spi_flags([base, base]), {"display_lcd": True})
            self.assertEqual(compat.requested_spi_flags([base, amend]), {"display_lcd": False})
            self.assertEqual(compat.requested_spi_flags([Path(directory)]), {})
            (amend / "board_devices.yaml").write_text(text.replace("true", '"true"'))
            with self.assertRaisesRegex(ValueError, "Invalid explicit SPI DMA flag"):
                compat.requested_spi_flags([amend])

    def test_cli_uses_actual_idf_header_and_repo_p4_yaml(self):
        with tempfile.TemporaryDirectory(prefix="board-manager-cli-") as directory:
            temporary = Path(directory)
            header = temporary / "components/esp_common/include/esp_idf_version.h"
            header.parent.mkdir(parents=True)
            header.write_text("#define ESP_IDF_VERSION_MAJOR 5\n#define ESP_IDF_VERSION_MINOR 5\n#define ESP_IDF_VERSION_PATCH 6\n")
            source = temporary / "gen_board_device_config.c"
            source.write_text(GENERATED)
            result = subprocess.run([sys.executable, str(ROOT / "tools/board_manager_compat.py"), "--generated", str(source),
                                     "--board-dir", str(ROOT / "boards/ESP32_P4X_EYE/bmgr"), "--idf-path", str(temporary)],
                                    env={**os.environ, "ESP_IDF_VERSION": "5.5"}, check=True, capture_output=True, text=True)
            self.assertEqual(json.loads(result.stdout), {"idf": "5.5.6", "psram_dma_direct_supported": True, "patched_devices": ["display_lcd"]})
            self.assertIn(".psram_dma_direct = true,", source.read_text())


if __name__ == "__main__":
    unittest.main()

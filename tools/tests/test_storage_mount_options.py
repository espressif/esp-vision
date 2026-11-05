# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise the production startup/VM mount plan under each board policy."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


class StorageMountOptions(unittest.TestCase):
    def test_optional_volumes_default_on_and_fault_isolation(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")
        storage = (ROOT / "platform/storage.c").read_text()
        vfs = (ROOT / "platform/storage_vfs.c").read_text()
        signatures = (
            "static void esp_vision_storage_set_flash_status(",
            "static const esp_partition_t *esp_vision_storage_find_flash_partition(",
            "static esp_err_t esp_vision_storage_sdcard_try_mount_policy(",
            "esp_err_t esp_vision_storage_sdcard_try_mount(",
            "esp_err_t esp_vision_storage_sdcard_auto_mount(",
            "esp_err_t esp_vision_storage_init(",
            "void esp_vision_storage_get_status(",
            "esp_err_t esp_vision_storage_retry(",
        )
        production = "\n".join(function(storage, signature) for signature in signatures)
        production += "\n" + function(vfs, "void esp_vision_storage_mount_micropython(")
        source = (ROOT / "tools/tests/storage_mount_options_host_test.c").read_text().replace(
            "/* PRODUCTION_FUNCTIONS */", production)
        with tempfile.TemporaryDirectory(prefix="ev-storage-mounts-") as temporary:
            unit = Path(temporary) / "mounts.c"
            executable = Path(temporary) / "mounts"
            unit.write_text(source)
            policies = (
                (None, None, None, None),  # Real header defaults: Flash on, SD off.
                (0, 0, 1, 0), (0, 1, 1, 0), (1, 0, 1, 0),
                (1, 1, 0, 0), (1, 1, 0, 1), (1, 1, 1, 1),
            )
            for flash, sd, flash_format, sd_format in policies:
                with self.subTest(flash=flash, sd=sd, flash_format=flash_format, sd_format=sd_format):
                    defines = [
                        f"-DEV_EXPECT_FLASH_FORMAT={1 if flash_format is None else flash_format}",
                        f"-DEV_EXPECT_SD_FORMAT={0 if sd_format is None else sd_format}",
                    ]
                    if flash is not None:
                        defines += [
                            f"-DESP_VISION_STORAGE_AUTO_MOUNT_FLASH={flash}",
                            f"-DESP_VISION_STORAGE_AUTO_MOUNT_SD={sd}",
                            f"-DESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM={flash_format}",
                            f"-DESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM={sd_format}",
                        ]
                    subprocess.run([
                        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", *defines,
                        f"-I{ROOT / 'tools/tests/storage_writer_service_stubs'}",
                        f"-I{ROOT / 'tools/tests/storage_provision_stubs'}",
                        f"-I{ROOT / 'platform'}", str(unit), "-o", str(executable),
                    ], check=True)
                    subprocess.run([str(executable)], check=True)

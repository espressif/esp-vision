# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Fault-inject the production SD host/VFS lifetime around the FatFs policy."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class StorageSdcardMountTests(unittest.TestCase):
    def test_mount_cleanup_and_uncertain_host_barriers(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")
        storage = (ROOT / "platform/storage.c").read_text()
        production = []
        for signature in (
            "static esp_err_t esp_vision_storage_unregister_sdcard(void)\n{",
            "static esp_err_t esp_vision_storage_mount_sdcard(bool format_if_no_filesystem)\n{",
        ):
            start = storage.index(signature)
            production.append(storage[start:storage.index("\n}", start) + 2])
        source = (ROOT / "tools/tests/storage_sdcard_mount_host_test.c").read_text().replace(
            "/* PRODUCTION_FUNCTIONS */", "\n".join(production))
        with tempfile.TemporaryDirectory(prefix="ev-sdcard-mount-") as temporary:
            unit = Path(temporary) / "mount.c"
            unit.write_text(source)
            for version in (50506, 60100):
                with self.subTest(version=version):
                    executable = Path(temporary) / f"mount-{version}"
                    subprocess.run([
                        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        f"-DESP_IDF_VERSION={version}", str(unit), "-o", str(executable),
                    ], check=True)
                    subprocess.run([str(executable)], check=True)

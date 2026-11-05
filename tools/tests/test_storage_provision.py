# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise production cold-boot provisioning with persistent NVS fault injection."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class StorageProvisionTests(unittest.TestCase):
    def test_non_destructive_mount_confirmation_and_interruption_boundaries(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")
        with tempfile.TemporaryDirectory(prefix="ev-storage-provision-") as temporary:
            executable = Path(temporary) / "storage_provision_test"
            subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'tools/tests/storage_provision_stubs'}",
                f"-I{ROOT / 'tools/tests/storage_writer_service_stubs'}",
                f"-I{ROOT / 'platform'}",
                str(ROOT / "tools/tests/storage_provision_host_test.c"),
                "-o", str(executable),
            ], check=True, cwd=ROOT)
            subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()

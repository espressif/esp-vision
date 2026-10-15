# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile/link the production per-volume writer service with host IDF fakes."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class StorageWriterServiceTests(unittest.TestCase):
    def test_production_writer_callbacks_and_vfs_transition_hooks(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        with tempfile.TemporaryDirectory(prefix="ev-storage-writer-service-") as temporary:
            executable = Path(temporary) / "storage_writer_service_test"
            subprocess.run([
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'tools/tests/storage_writer_service_stubs'}",
                f"-I{ROOT / 'platform'}",
                str(ROOT / "tools/tests/storage_writer_service_host_test.c"),
                "-o",
                str(executable),
            ], check=True, cwd=ROOT)
            for scenario in (
                "transition",
                "legacy-eject-load-reboot",
                "usb-multichunk",
                "usb-empty-complete",
                "host-write-failure",
                "stale-transition-quarantine",
                "session-change",
                "open-timeout",
            ):
                subprocess.run([str(executable), scenario], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile and exercise the production per-volume MSC writer gate natively."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class StorageWriterGateTests(unittest.TestCase):
    def test_writer_lease_blocks_io_and_quarantines_lost_sessions(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        with tempfile.TemporaryDirectory(prefix="ev-storage-writer-") as temporary:
            executable = Path(temporary) / "storage_writer_gate_test"
            subprocess.run([
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'platform'}",
                str(ROOT / "tools/tests/storage_writer_gate_host_test.c"),
                "-o",
                str(executable),
            ], check=True, cwd=ROOT)
            subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()

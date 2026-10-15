#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile and exercise the production VM output scope with host session stubs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class EvVmOutputNativeTests(unittest.TestCase):
    def test_production_vm_output_scope(self) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")
        with tempfile.TemporaryDirectory(prefix="ev-vm-output-test-") as temporary:
            executable = Path(temporary) / "ev_vm_output_host_test"
            command = [
                compiler,
                "-std=c11",
                "-D_POSIX_C_SOURCE=200809L",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
                f"-I{ROOT / 'tools/tests/ev_session_stubs'}",
                f"-I{ROOT / 'platform'}",
                str(ROOT / "platform/ev_vm_output.c"),
                str(ROOT / "tools/tests/ev_vm_output_host_test.c"),
                "-o",
                str(executable),
            ]
            subprocess.run(command, check=True, cwd=ROOT)
            subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()

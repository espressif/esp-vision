#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise EV-MUX startup failure behavior without a board or ESP-IDF build."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class EvStdioStartupFaultTest(unittest.TestCase):
    def test_native_mux_oom_preserves_diagnostics_and_retries(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        source = ROOT / "tools/tests/test_ev_stdio_startup_faults.c"
        stubs = ROOT / "tools/tests/ev_mux_tx_stubs"
        with tempfile.TemporaryDirectory(prefix="ev-stdio-startup-") as temp_dir:
            binary = pathlib.Path(temp_dir) / "test_ev_stdio_startup_faults"
            subprocess.run(
                [
                    compiler,
                    "-std=gnu11",
                    "-Wall",
                    "-Werror",
                    "-Wno-unused-function",
                    "-Wno-unused-variable",
                    f"-I{stubs}",
                    f"-I{ROOT / 'platform'}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(binary)], check=True, cwd=ROOT)

if __name__ == "__main__":
    unittest.main()

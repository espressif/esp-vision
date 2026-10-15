#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile and exercise the production launcher state machine with host stubs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class EvLauncherStateNativeTests(unittest.TestCase):
    def test_production_launcher_state_guards_and_reset_identity(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        with tempfile.TemporaryDirectory(prefix="ev-launcher-state-") as temporary:
            executable = Path(temporary) / "ev_launcher_state_test"
            subprocess.run([
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'tools/tests/ev_launcher_state_stubs'}",
                f"-I{ROOT / 'platform'}",
                str(ROOT / "platform/ev_launcher_state.c"),
                str(ROOT / "tools/tests/ev_launcher_state_host_test.c"),
                "-o",
                str(executable),
            ], check=True, cwd=ROOT)
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    def test_candidate_module_helper_compiles_against_real_micropython_headers(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")
        micropython = ROOT / "lib/micropython"
        generated = micropython / "mpy-cross/build/genhdr"
        if not (generated / "qstrdefs.generated.h").is_file():
            self.skipTest("generated MicroPython headers are unavailable; full firmware build is intentionally not run here")

        subprocess.run([
            compiler,
            "-std=gnu99",
            "-fsyntax-only",
            "-Wall",
            "-Wextra",
            "-Wno-unused-parameter",
            "-Wno-missing-field-initializers",
            "-DMP_QSTR_esp_vision_launcher=300",
            "-DMP_QSTR_execute_active=301",
            "-DMP_QSTR_execute_trial=302",
            "-DMP_QSTR__new_candidate_module=303",
            f"-I{micropython / 'mpy-cross'}",
            f"-I{micropython / 'mpy-cross/build'}",
            f"-I{generated}",
            f"-I{micropython}",
            f"-I{micropython / 'py'}",
            f"-I{ROOT / 'platform'}",
            str(ROOT / "modules/py_esp_vision_launcher.c"),
        ], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()

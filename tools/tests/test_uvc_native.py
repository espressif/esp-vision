# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile and exercise the production UVC state machine against local stubs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class UvcNativeBehaviorTests(unittest.TestCase):
    def test_stream_modes_and_inflight_behavior(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        with tempfile.TemporaryDirectory(prefix="ev-uvc-native-") as temporary:
            executable = Path(temporary) / "uvc_native_host_test"
            disabled_executable = Path(temporary) / "uvc_disabled_host_test"
            subprocess.run([
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'tools/tests/uvc_stubs'}",
                f"-I{ROOT / 'platform'}",
                str(ROOT / "platform/uvc.c"),
                str(ROOT / "tools/tests/uvc_native_host_test.c"),
                "-Wl,--wrap=free",
                "-o",
                str(executable),
            ], check=True, cwd=ROOT)

            subprocess.run([
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-DCFG_TUD_VIDEO=0",
                f"-I{ROOT / 'tools/tests/uvc_stubs'}",
                f"-I{ROOT / 'platform'}",
                str(ROOT / "platform/uvc.c"),
                str(ROOT / "tools/tests/uvc_disabled_host_test.c"),
                "-o",
                str(disabled_executable),
            ], check=True, cwd=ROOT)

            for scenario in ("frame-lifetime", "modes-and-epochs"):
                subprocess.run([str(executable), scenario], check=True, cwd=ROOT)
            subprocess.run([str(disabled_executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()

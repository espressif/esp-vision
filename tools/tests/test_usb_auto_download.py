#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Run the production P4 CDC auto-download path with host register/timer stubs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class UsbAutoDownloadTests(unittest.TestCase):
    def test_p4_download_normal_and_dtr_only_paths(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        with tempfile.TemporaryDirectory(prefix="usb-auto-download-test-") as temporary:
            source = (ROOT / "overlay/micropython/ports/esp32/modmachine.c").read_text()
            start = source.index("MP_NORETURN static void machine_bootloader_rtc(void) {")
            end = source.index("\n}\n", start) + 2
            (Path(temporary) / "machine_bootloader_test.inc").write_text(source[start:end])
            executable = Path(temporary) / "usb_auto_download_host_test"
            subprocess.run([
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{temporary}",
                f"-I{ROOT / 'tools/tests/usb_auto_download_stubs'}",
                f"-I{ROOT / 'platform'}",
                str(ROOT / "tools/tests/usb_auto_download_host_test.c"),
                "-o",
                str(executable),
            ], check=True, cwd=ROOT)
            for scenario in ("boot", "normal", "dtr", "machine", "machine-no-init"):
                subprocess.run([str(executable), scenario], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()

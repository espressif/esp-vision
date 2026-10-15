#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile and exercise the production EV-MUX channel route with host stubs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class EvChannelNativeTests(unittest.TestCase):
    def test_repeated_init_preserves_owner_and_release_restores_fallback(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        stubs = ROOT / "tools/tests/ev_channel_stubs"
        with tempfile.TemporaryDirectory(prefix="ev-channel-native-") as temporary:
            executable = Path(temporary) / "ev_channel_host_test"
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    "-ffunction-sections",
                    "-fdata-sections",
                    f"-I{stubs}",
                    f"-I{ROOT / 'tools/tests/ev_session_stubs'}",
                    f"-I{ROOT / 'platform'}",
                    str(ROOT / "platform/ev_channel.c"),
                    str(ROOT / "tools/tests/ev_channel_host_test.c"),
                    "-Wl,--gc-sections",
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run(
                [str(executable)], check=True, capture_output=True, text=True
            )
            self.assertIn("owner survives repeated init", result.stdout)


if __name__ == "__main__":
    unittest.main()

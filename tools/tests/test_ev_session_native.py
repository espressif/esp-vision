#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile and execute the production ev_session.c with deterministic host stubs."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def run_host_test() -> None:
    with tempfile.TemporaryDirectory(prefix="ev-session-test-") as temporary:
        executable = Path(temporary) / "ev_session_host_test"
        command = [
            "cc",
            "-std=c11",
            "-D_POSIX_C_SOURCE=200809L",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            f"-I{ROOT / 'tools/tests/ev_session_stubs'}",
            f"-I{ROOT / 'platform'}",
            str(ROOT / "platform/ev_session.c"),
            str(ROOT / "tools/tests/ev_session_host_test.c"),
            "-o",
            str(executable),
        ]
        subprocess.run(command, check=True, cwd=ROOT)
        subprocess.run([str(executable)], check=True, cwd=ROOT)


class EvSessionNativeTests(unittest.TestCase):
    def test_production_session_state_machine(self) -> None:
        run_host_test()


if __name__ == "__main__":
    unittest.main()

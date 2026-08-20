#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile and run host regressions for the ESP-VISION imlib allocators."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    compiler = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="esp-vision-imlib-test-") as tmp:
        binary = Path(tmp) / "imlib_allocator_test"
        command = [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'tools/host_tests/include'}",
            f"-I{ROOT / 'components/imlib/include'}",
            str(ROOT / "tools/host_tests/imlib_allocator_test.c"),
            str(ROOT / "components/imlib/compat/fb_alloc.c"),
            str(ROOT / "components/imlib/compat/umm_malloc.c"),
            "-o",
            str(binary),
        ]
        subprocess.run(command, check=True)
        subprocess.run([binary], check=True)


if __name__ == "__main__":
    main()

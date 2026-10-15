# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Compile and exercise the production EV-MUX metadata/hello JSON parser."""

import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tools/tests/fixtures/evmux_v4_strict_json.json"


class EvControlJsonTests(unittest.TestCase):
    def test_native_parser_and_raw_fixtures(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")
        with tempfile.TemporaryDirectory(prefix="ev-control-json-") as directory:
            binary = pathlib.Path(directory) / "ev_control_json_test"
            subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'platform'}",
                str(ROOT / "tools/tests/ev_control_json_test.c"),
                "-o", str(binary),
            ], check=True, cwd=ROOT)
            subprocess.run([str(binary)], check=True, cwd=ROOT)

            fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
            for kind in ("metadata", "hello", "rpcPayload"):
                for case in fixture[kind]:
                    with self.subTest(kind=kind, name=case["name"]):
                        subprocess.run([
                            str(binary), kind, str(case["valid"]).lower(), case["json"],
                        ], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()

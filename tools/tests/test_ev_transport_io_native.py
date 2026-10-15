# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Deterministic generation-change tests against the production transport."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class EvTransportIoNativeTests(unittest.TestCase):
    def test_serial_write_is_serialized_with_close_and_generation_advance(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        stubs = ROOT / "tools/tests/ev_transport_io_stubs"
        with tempfile.TemporaryDirectory(prefix="ev-transport-io-") as directory:
            executable = pathlib.Path(directory) / "ev_transport_io_test"
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    "-DMICROPY_HW_ESP_USB_SERIAL_JTAG=1",
                    "-DMICROPY_HW_USB_CDC=0",
                    "-DMICROPY_HW_ENABLE_UART_REPL=0",
                    f"-I{stubs}",
                    f"-I{ROOT / 'platform'}",
                    str(ROOT / "platform/ev_transport.c"),
                    str(ROOT / "tools/tests/ev_transport_io_host_test.c"),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run(
                [str(executable)], check=True, capture_output=True, text=True
            )
            self.assertIn("serial write generation gate passed", result.stdout)


if __name__ == "__main__":
    unittest.main()

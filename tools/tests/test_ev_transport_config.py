# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Compile board transport assertions with ESP32 port macro combinations."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class EvTransportConfigTests(unittest.TestCase):
    def test_undefined_uart_and_port_usb_defaults(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")
        source = ROOT / "tools/tests/ev_transport_config_test.c"
        stubs = ROOT / "tools/tests/ev_transport_config_stubs"
        platform = ROOT / "platform"
        esp32_port = ROOT / "lib/micropython/ports/esp32"

        cases = [
            # S31-CHATBOT: USBDEV=0, USJ explicitly enabled, UART omitted.
            ["-DMICROPY_HW_ENABLE_USBDEV=0", "-DMICROPY_HW_ESP_USB_SERIAL_JTAG=1",
             "-DEV_CONFIG_TEST_EXPECT_UART_REPL=0"],
            # USB-OTG CDC inherits MICROPY_HW_ENABLE_USBDEV from mpconfigport defaults.
            ["-DMICROPY_HW_ENABLE_USBDEV=1", '-DEV_CONFIG_TEST_DEFAULT_SINK="usb"',
             "-DEV_CONFIG_TEST_EXPECT_UART_REPL=0"],
            # An explicit board setting is honored.
            ["-DMICROPY_HW_ENABLE_USBDEV=0", "-DMICROPY_HW_ENABLE_UART_REPL=1",
             '-DEV_CONFIG_TEST_DEFAULT_SINK="uart"', "-DEV_CONFIG_TEST_EXPECT_UART_REPL=1"],
            # With USB-OTG CDC and USJ both off, uart.h defaults UART REPL on.
            ["-DMICROPY_HW_ENABLE_USBDEV=0", "-DMICROPY_HW_ESP_USB_SERIAL_JTAG=0",
             '-DEV_CONFIG_TEST_DEFAULT_SINK="uart"', "-DEV_CONFIG_TEST_EXPECT_UART_REPL=1"],
        ]
        with tempfile.TemporaryDirectory(prefix="ev-transport-config-") as directory:
            output = pathlib.Path(directory) / "transport_config.o"
            for defines in cases:
                with self.subTest(defines=defines):
                    subprocess.run([
                        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-c",
                        f"-I{stubs}", f"-I{platform}", f"-I{esp32_port}", *defines,
                        str(source), "-o", str(output),
                    ], check=True, cwd=ROOT)

            unsupported_uart = subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-c",
                f"-I{stubs}", f"-I{platform}", "-DMICROPY_HW_ENABLE_USBDEV=0",
                f"-I{esp32_port}", "-DMICROPY_HW_ESP_USB_SERIAL_JTAG=0",
                "-DMICROPY_HW_ENABLE_UART_REPL=0", '-DEV_CONFIG_TEST_DEFAULT_SINK="uart"',
                "-DEV_CONFIG_TEST_EXPECT_UART_REPL=0",
                str(source), "-o", str(output),
            ], check=False, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(unsupported_uart.returncode, 0)
            self.assertIn("EV-MUX default must name an enabled physical serial endpoint",
                          unsupported_uart.stderr)


if __name__ == "__main__":
    unittest.main()

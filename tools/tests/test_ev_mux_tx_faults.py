# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class EvMuxTxFaultInjectionTest(unittest.TestCase):
    def test_native_scheduler_faults(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        source = ROOT / "tools/tests/test_ev_mux_tx_faults.c"
        stubs = ROOT / "tools/tests/ev_mux_tx_stubs"
        for spiram_enabled in (False, True):
            with self.subTest(config_spiram=spiram_enabled):
                with tempfile.TemporaryDirectory(prefix="ev-mux-tx-") as temp_dir:
                    binary = pathlib.Path(temp_dir) / "test_ev_mux_tx_faults"
                    subprocess.run(
                        [
                            compiler,
                            "-std=gnu11",
                            "-Wall",
                            "-Werror",
                            "-Wno-unused-function",
                            "-Wno-unused-variable",
                            f"-DCONFIG_SPIRAM={int(spiram_enabled)}",
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

    def test_mphalport_does_not_fall_back_to_raw_io_when_mux_is_enabled(self):
        source = (ROOT / "overlay/micropython/ports/esp32/mphalport.c").read_text()
        start = source.index("mp_uint_t mp_hal_stdout_tx_strn")
        end = source.index("mp_uint_t mp_hal_ticks_ms", start)
        function = source[start:end]
        mux_guard = re.search(
            r"if\s*\(ev_stdio_mux_enabled\(\)\)\s*\{\s*"
            r"return\s+ev_stdio_write_repl_stdout\(str,\s*len\)\s*\?\s*len\s*:\s*0;\s*\}",
            function,
        )
        self.assertIsNotNone(mux_guard)
        self.assertLess(mux_guard.end(), function.index("usb_serial_jtag_tx_strn"))
        self.assertLess(mux_guard.end(), function.index("uart_stdout_tx_strn"))
        self.assertLess(mux_guard.end(), function.index("mp_usbd_cdc_tx_strn"))


if __name__ == "__main__":
    unittest.main()

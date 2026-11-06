#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Run production P4/S31 CDC auto-download with host register/timer stubs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class UsbAutoDownloadTests(unittest.TestCase):
    def test_p4_s31_download_normal_and_dtr_only_paths(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        with tempfile.TemporaryDirectory(prefix="usb-auto-download-test-") as temporary:
            source = (ROOT / "overlay/micropython/ports/esp32/modmachine.c").read_text()
            start = source.index("MP_NORETURN static void machine_bootloader_rtc(void) {")
            end = source.index("\n}\n", start) + 2
            (Path(temporary) / "machine_bootloader_test.inc").write_text(source[start:end])
            executable = Path(temporary) / "usb_auto_download_host_test"
            for target in ("ESP32P4", "ESP32S31"):
                with self.subTest(target=target):
                    subprocess.run([
                        compiler,
                        "-std=c11",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        f"-DCONFIG_IDF_TARGET_{target}=1",
                        f"-I{temporary}",
                        f"-I{ROOT / 'tools/tests/usb_auto_download_stubs'}",
                        f"-I{ROOT / 'platform'}",
                        str(ROOT / "tools/tests/usb_auto_download_host_test.c"),
                        "-o",
                        str(executable),
                    ], check=True, cwd=ROOT)
                    for scenario in ("boot", "normal", "dtr", "machine", "machine-no-init", "shutdown-order", "registration", "registration-full", "auto-init-full", "control-full", "control-coalesced", "false-false-open"):
                        with self.subTest(scenario=scenario):
                            subprocess.run([str(executable), scenario], check=True, cwd=ROOT, timeout=5)
                    if target == "ESP32S31":
                        with self.assertRaises(subprocess.TimeoutExpired) as stopped:
                            subprocess.run([str(executable), "rom-return"], check=True, cwd=ROOT, stdout=subprocess.PIPE, timeout=0.25)
                        self.assertIn(b"rom-reset-returned", stopped.exception.stdout)

    def test_s31_registers_shutdown_before_usb_machine_and_network_startup(self) -> None:
        source = (ROOT / "platform/main.c").read_text()
        task = source[source.index("void mp_task(void *pvParameter)"):]
        register = task.index("ESP_ERROR_CHECK(esp_vision_restart_init());")
        self.assertIn("#if CONFIG_IDF_TARGET_ESP32S31", task[:register])
        for initialization in ("usb_serial_jtag_init();", "usb_phy_init();", "machine_init();", "esp_event_loop_create_default();"):
            self.assertLess(register, task.index(initialization))


if __name__ == "__main__":
    unittest.main()

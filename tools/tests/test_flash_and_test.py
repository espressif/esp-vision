# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Verify the HIL driver delegates all device communication to the public SDK."""

import argparse
import contextlib
import io
import subprocess
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

CI_DIR = Path(__file__).resolve().parents[1] / "ci"
sys.path.insert(0, str(CI_DIR))
import flash_and_test


class FlashAndTestTests(unittest.TestCase):
    def args(self, **changes):
        return argparse.Namespace(node="node22", role="usj", port="/dev/fake", baudrate=115200,
                                  board="TEST", expected_version="build-version", require_all=True,
                                  checks="all", frames=30, display_hold=0, output="logs/frame.jpg", screen_output="logs/screen.jpg",
                                  boot_timeout=5, **changes)

    def test_port_descriptors_select_usb_or_usj_without_protocol_probes(self):
        ports = SimpleNamespace(comports=lambda: [
            SimpleNamespace(device="/dev/a", product="ESP-VISION", description="CDC"),
            SimpleNamespace(device="/dev/b", product="USB JTAG serial debug unit", description=""),
        ])
        with patch.object(flash_and_test, "list_ports", ports):
            self.assertEqual(flash_and_test.serial_role("/dev/a"), "usb")
            self.assertEqual(flash_and_test.serial_role("/dev/b"), "usj")
            self.assertEqual(flash_and_test.serial_role("/dev/a", "uart"), "uart")

    def test_probe_and_full_check_share_sdk_cli_and_python_environment(self):
        args = self.args()
        full = flash_and_test.hardware_check_command(args, "/dev/fake")
        probe = flash_and_test.hardware_check_command(args, "/dev/fake", probe=True, timeout_ms=2000)
        for command in (full, probe):
            self.assertEqual(command[0], "node22")
            self.assertEqual(Path(command[1]), flash_and_test.REPO_ROOT / "host/device-core/tests/hardware-check.mjs")
            self.assertEqual(command[command.index("--serial") + 1], "/dev/fake")
            self.assertEqual(command[command.index("--python") + 1], sys.executable)
            self.assertEqual(command[command.index("--role") + 1], "usj")
            self.assertEqual(command[command.index("--expect-board") + 1], "TEST")
            self.assertEqual(command[command.index("--expect-firmware-version") + 1], "build-version")
        self.assertNotIn("--checks", probe)
        self.assertNotIn("--require-all", probe)
        self.assertNotIn("--screen-output", probe)
        self.assertIn("--require-all", full)
        self.assertEqual(probe[-2:], ["--timeout", "2000"])
        for option, value in (("--checks", "all"), ("--frames", "30"),
                              ("--display-hold", "0"), ("--output", "logs/frame.jpg"),
                              ("--screen-output", "logs/screen.jpg")):
            self.assertEqual(full[full.index(option) + 1], value)

    def test_wait_retries_sdk_failures_and_preserves_diagnostic_output(self):
        results = [SimpleNamespace(returncode=1, stdout='{"ok":false,"code":"WRITE_FAILED","mounted":false}\n'),
                   SimpleNamespace(returncode=0, stdout='{"check":"python","ok":true}\n')]
        output = io.StringIO()
        with patch.object(flash_and_test, "candidate_ports", return_value=["/dev/a", "/dev/b"]), \
                patch.object(flash_and_test.subprocess, "run", side_effect=results) as runner, \
                contextlib.redirect_stdout(output):
            self.assertEqual(flash_and_test.wait_for_device(self.args()), "/dev/b")
        self.assertEqual(runner.call_count, 2)
        self.assertIn('"code":"WRITE_FAILED"', output.getvalue())
        self.assertIn('"mounted":false', output.getvalue())
        self.assertIn('"check":"python"', output.getvalue())
        self.assertEqual(runner.call_args.args[0][1], str(flash_and_test.HARDWARE_CHECK))

    def test_wait_handles_cli_timeout_then_tries_next_port(self):
        timeout = subprocess.TimeoutExpired(["node22"], 1, output=b'{"code":"TIMEOUT"}\n')
        results = [timeout, SimpleNamespace(returncode=0, stdout="ready\n")]
        with patch.object(flash_and_test, "candidate_ports", return_value=["/dev/a", "/dev/b"]), \
                patch.object(flash_and_test.subprocess, "run", side_effect=results), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(flash_and_test.wait_for_device(self.args()), "/dev/b")
        self.assertIn('"code":"TIMEOUT"', output.getvalue())

    def test_nondefault_baudrate_is_rejected_before_flashing(self):
        with patch.object(sys, "argv", ["flash_and_test.py", "--board", "test", "--baudrate", "9600"]), \
                patch.object(flash_and_test, "run") as runner, \
                contextlib.redirect_stderr(io.StringIO()) as output, \
                self.assertRaises(SystemExit) as raised:
            flash_and_test.main()
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("requires --baudrate 115200", output.getvalue())
        runner.assert_not_called()

    def test_failed_sdk_checks_fail_the_hil_job_after_successful_flash(self):
        tee = SimpleNamespace(console=io.StringIO(), close=lambda: None)
        build = MagicMock()
        (build / "flasher_args.json").read_text.return_value = "{}"
        (build / "project_description.json").read_text.return_value = '{"project_version":"test"}'
        commands = []

        def run(command):
            commands.append(command)
            return 1 if command[1:2] == [str(flash_and_test.HARDWARE_CHECK)] else 0

        with patch.object(sys, "argv", ["flash_and_test.py", "--board", "test"]), \
                patch.object(flash_and_test, "Tee", return_value=tee), \
                patch.object(flash_and_test, "serial", SimpleNamespace()), \
                patch.object(flash_and_test.shutil, "which", return_value="node"), \
                patch.object(flash_and_test.subprocess, "run", return_value=SimpleNamespace(returncode=0)), \
                patch.object(flash_and_test, "resolve_build_dir", return_value=build), \
                patch.object(flash_and_test, "print_context"), \
                patch.object(flash_and_test, "candidate_ports", return_value=["/dev/fake"]), \
                patch.object(flash_and_test, "flash_command", return_value=["esptool", "write-flash"]), \
                patch.object(flash_and_test, "wait_for_device", return_value="/dev/fake"), \
                patch.object(flash_and_test.time, "sleep"), \
                patch.object(flash_and_test, "run", side_effect=run):
            # Preserve the test runner's stdout: main normally owns its Tee.
            with patch.object(sys, "stdout", io.StringIO()):
                # A writable stand-in captures the full stage log.
                tee.write = lambda text: len(text)
                tee.flush = lambda: None
                self.assertEqual(flash_and_test.main(), 1)
        self.assertEqual(commands[0], ["esptool", "write-flash"])
        self.assertEqual(commands[1][1], str(flash_and_test.HARDWARE_CHECK))
        self.assertEqual(len(commands), 2)

    def test_driver_import_does_not_require_pyserial_or_legacy_mux_clients(self):
        result = subprocess.run([
            sys.executable, "-S", "-c",
            f"import sys; sys.path.insert(0, {str(CI_DIR)!r}); import flash_and_test; "
            "assert 'device_checks' not in sys.modules; assert 'evmux_v4' not in sys.modules; "
            "assert flash_and_test.HARDWARE_CHECK.name == 'hardware-check.mjs'",
        ], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()

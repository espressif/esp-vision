# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Verify the HIL driver delegates all device communication to the public SDK."""

import argparse
import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

CI_DIR = Path(__file__).resolve().parents[1] / "ci"
sys.path.insert(0, str(CI_DIR))
import flash_and_test


class FlashAndTestTests(unittest.TestCase):
    def test_ambiguous_ports_and_failed_flash_verification_prevent_runtime_checks(self):
        for ports, expected in ((["/dev/a", "/dev/b"], []), (["/dev/a"], ["write", "verify"])):
            with self.subTest(ports=ports), tempfile.TemporaryDirectory() as directory:
                build = Path(directory)
                (build / "flasher_args.json").write_text('{}')
                (build / "project_description.json").write_text('{"project_version":"test"}')
                commands = []

                def run(command):
                    commands.append(command[0])
                    return int(command[0] == "verify")

                argv = ["flash_and_test.py", "--board", "TEST", "--verify-flash", "--port", "",
                        "--log", str(build / "target.log"), "--junit", str(build / "target.xml")]
                with patch.object(sys, "argv", argv), \
                        patch.object(flash_and_test, "serial", SimpleNamespace()), \
                        patch.object(flash_and_test.shutil, "which", return_value="node"), \
                        patch.object(flash_and_test.subprocess, "run", return_value=SimpleNamespace(returncode=0)), \
                        patch.object(flash_and_test, "resolve_build_dir", return_value=build), \
                        patch.object(flash_and_test, "print_context"), \
                        patch.object(flash_and_test, "candidate_ports", return_value=ports), \
                        patch.object(flash_and_test, "flash_command", return_value=["write"]), \
                        patch.object(flash_and_test, "verify_command", return_value=["verify"]), \
                        patch.object(flash_and_test, "wait_for_device") as ready, \
                        patch.object(flash_and_test, "run", side_effect=run), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(flash_and_test.main(), 1)
                    ready.assert_not_called()
                self.assertEqual(commands, expected)
                self.assertGreater(int(ET.parse(build / "target.xml").getroot().attrib["failures"]), 0)

    def test_verify_flash_checks_every_image_with_the_same_header_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            for name in ("boot.bin", "app.bin"):
                (build / name).write_bytes(b"firmware")
            flasher = {"extra_esptool_args": {"chip": "esp32p4"},
                       "flash_settings": {"flash_mode": "dio", "flash_freq": "80m", "flash_size": "16MB"},
                       "flash_files": {"0x10000": "app.bin", "0x2000": "boot.bin"}}
            args = argparse.Namespace(chip=None, port="/dev/selected", baud=460800)
            for separator in ("-", "_"):
                names = {"write": "write" + separator + "flash", "separator": separator}
                with patch.object(flash_and_test, "esptool_names", return_value=names):
                    command = flash_and_test.verify_command(args, build, flasher)
                    self.assertIn("verify" + separator + "flash", command)
                    self.assertNotIn(names["write"], command)
                    self.assertEqual(command[-4:], ["0x2000", str(build / "boot.bin"), "0x10000", str(build / "app.bin")])
                    self.assertEqual(command[command.index("--flash" + separator + "mode") + 1], "dio")
                    self.assertEqual(command[command.index("--port") + 1], "/dev/selected")
            with patch.object(flash_and_test, "esptool_names", return_value=names):
                with self.assertRaisesRegex(SystemExit, "no flash images"):
                    flash_and_test.flash_command(args, build, {})
                (build / "app.bin").unlink()
                with self.assertRaisesRegex(SystemExit, "missing flash image"):
                    flash_and_test.verify_command(args, build, flasher)

    def test_junit_preserves_check_failures_skips_and_early_abort(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.xml"
            checks = [{"check": "discovery", "status": "PASS", "data": {"probe": False}},
                      {"check": "modules", "status": "FAIL", "error": {"message": "a < b & c"}},
                      {"check": "ulab", "status": "SKIP", "error": {"message": "unavailable"}}]
            flash_and_test.write_junit(path, "TEST", [flash_and_test.Stage("flash", "PASS", 1)], checks, True, 2, "incomplete")
            suite = ET.parse(path).getroot()
            self.assertEqual(suite.attrib["tests"], "5")
            self.assertEqual(suite.attrib["failures"], "2")
            self.assertEqual(suite.attrib["skipped"], "1")
            self.assertIn("a < b & c", suite.find("./testcase/failure").attrib["message"])
            flash_and_test.write_junit(path, "TEST", [], [], True, 0, "no device")
            self.assertEqual(ET.parse(path).find("./testcase/failure").attrib["message"], "no device")

    def test_tee_collects_only_complete_check_records(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            tee = flash_and_test.Tee(Path(directory) / "target.log")
            try:
                tee.write('esptool output\n{"check":"python",')
                tee.write('"ok":true}\n{"check":"summary","passed":1}\n')
                self.assertEqual(tee.checks, [{"check": "python", "ok": True}])
            finally:
                tee.close()

    def test_junit_distinguishes_bootstrap_events_from_blocked_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.xml"
            events = [{"check": "fixture-bootstrap", "event": event}
                      for event in ("retry", "ready")]
            flash_and_test.write_junit(path, "TEST", [], events + [
                {"check": "modules", "status": "PASS"}], False, 1)
            suite = ET.parse(path).getroot()
            self.assertEqual(suite.attrib["tests"], "1")
            self.assertEqual(suite.attrib["failures"], "0")
            flash_and_test.write_junit(path, "TEST", [], events + [
                {"check": "fixture-bootstrap", "status": "FAIL"},
                {"check": "modules", "status": "BLOCKED", "blockedBy": "fixture-bootstrap"},
                {"check": "ulab", "status": "UNKNOWN"}], True, 1)
            suite = ET.parse(path).getroot()
            self.assertEqual(suite.attrib["tests"], "4")
            self.assertEqual(suite.attrib["failures"], "4")
            self.assertIn('"blockedBy": "fixture-bootstrap"',
                          suite.findall("testcase")[1].find("system-out").text)

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

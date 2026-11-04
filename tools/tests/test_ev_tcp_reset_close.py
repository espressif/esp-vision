# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Compile production reset/close functions with descriptor and RTOS faults."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    end = source.index("\n}", source.index("{", start)) + 2
    return source[start:end]


class TcpResetCloseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            raise unittest.SkipTest("native C compiler unavailable")
        cls.directory = tempfile.TemporaryDirectory(prefix="ev-tcp-reset-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "reset_test"
        source = (ROOT / "platform/ev_tcp.c").read_text()
        control = (ROOT / "platform/ev_control_transport.c").read_text()
        types = source[source.index("typedef struct {"):source.index("} ev_tcp_link_t;") + len("} ev_tcp_link_t;")]
        defines = "\n".join(re.findall(r"^#define EV_TCP_RESET_(?:LINGER_SECONDS|LOCK_TIMEOUT_MS) .*", source, re.MULTILINE))
        tcp = "\n".join(function(source, signature) for signature in (
            "static bool ev_tcp_close_requested(",
            "static bool ev_tcp_link_matches_locked(",
            "esp_err_t ev_tcp_link_close_for_reset(",
            "static void ev_tcp_close_pending_links(void)",
        ))
        reset = function(control, "static esp_err_t ev_control_send_reset(uint32_t seq)\n{")
        template = (ROOT / "tools/tests/ev_tcp_reset_close_test.c").read_text()
        generated = template.replace("/* PRODUCTION_TYPES */", types)
        generated = generated.replace("/* PRODUCTION_TCP_FUNCTIONS */", defines + "\n" + tcp)
        generated = generated.replace("/* PRODUCTION_RESET_FUNCTION */", reset)
        cfile = Path(cls.directory.name) / "reset_test.c"
        cfile.write_text(generated)
        built = subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{ROOT / 'tools/tests/ev_mux_tx_stubs'}", f"-I{ROOT / 'platform'}",
            str(cfile), "-o", str(cls.binary),
        ], cwd=ROOT, capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def run_scenarios(self, *scenarios):
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.binary), scenario], cwd=ROOT,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_descriptor_detached_and_closed_outside_lock(self):
        self.run_scenarios("success", "already_blocking")

    def test_invalid_or_stale_links_do_not_close_another_connection(self):
        self.run_scenarios("invalid_arguments", "stale_generation", "lock_failure")

    def test_setup_and_close_errors_are_retained_with_one_close_attempt(self):
        self.run_scenarios("get_flags_failure", "set_flags_failure", "linger_failure",
                           "close_failure", "setup_and_close_failure", "fallback_failure")

    def test_pending_close_and_generation_reuse_cannot_double_close(self):
        self.run_scenarios("pending_close_wins", "late_pending_close", "slot_reused_during_close")

    def test_ordinary_close_does_not_enable_linger(self):
        self.run_scenarios("ordinary_close_unchanged")

    def test_reset_checks_ticket_outcomes_and_restarts_once_without_retry(self):
        self.run_scenarios("reset_enqueue_failure", "reset_missing_ticket", "reset_wait_timeout",
                           "reset_transmit_failure", "reset_stale_generation")

    def test_successful_tcp_ticket_closes_while_serial_paths_do_not(self):
        self.run_scenarios("reset_tcp", "reset_usj", "reset_usb", "reset_uart")

    def test_reset_keeps_local_close_failure_visible(self):
        self.run_scenarios("reset_local_setup_failure", "reset_local_close_failure", "reset_fallback_failure")

    def test_shared_defaults_enable_pinned_sdk_linger_support(self):
        defaults = (ROOT / "overlay/micropython/ports/esp32/boards/sdkconfig.esp_vision_network").read_text()
        self.assertIn("CONFIG_LWIP_SO_LINGER=y", defaults)
        source = (ROOT / "platform/ev_tcp.c").read_text()
        self.assertIn('_Static_assert(LWIP_SO_LINGER,', source)


if __name__ == "__main__":
    unittest.main()

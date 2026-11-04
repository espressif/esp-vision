# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Exercise the examples' real WiFi functions without importing their applications."""

import ast
import errno
from pathlib import Path
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = (
    "example/01-Camera/03-MJPEG/wifi_mjpeg_stream.py",
    "example/07-Network/01-Cloud-AI/openai_compatible_vision.py",
)
TARGET = "example-test-target"


class Clock:
    PERIOD = 65536

    def __init__(self, start=0):
        self.start = start
        self.elapsed = 0
        self.sleeps = []

    def ticks_ms(self):
        return (self.start + self.elapsed) % self.PERIOD

    def ticks_add(self, value, delta):
        return (value + delta) % self.PERIOD

    def ticks_diff(self, first, second):
        half = self.PERIOD // 2
        return (first - second + half) % self.PERIOD - half

    def sleep_ms(self, delay):
        if delay <= 0:
            raise AssertionError("A waiting example must yield for a positive interval")
        self.sleeps.append(delay)
        self.elapsed += delay
        if self.elapsed > 50000:
            raise AssertionError("WiFi retry budget was exceeded")


class Wlan:
    def __init__(self, clock):
        self.clock = clock
        self.active_state = True
        self.online_at = None
        self.ssid = TARGET
        self.managed_connecting = False
        self.connect_delay = 400
        self.connect_errors = []
        self.connect_error = None
        self.active_errors = []
        self.ssid_error = None
        self.activations = []
        self.connections = []
        self.config_queries = []

    def isconnected(self):
        return self.online_at is not None and self.clock.elapsed >= self.online_at

    def config(self, name):
        self.config_queries.append(name)
        if name != "ssid":
            raise AssertionError("Only the read-only SSID query is expected")
        if self.ssid_error:
            raise self.ssid_error
        return self.ssid

    def active(self, value=None):
        if value is None:
            return self.active_state
        self.activations.append(value)
        if value is False:
            raise AssertionError("The example must not disconnect an existing STA")
        if self.active_errors:
            raise self.active_errors.pop(0)
        self.active_state = value

    def status(self):
        return 1 if self.managed_connecting else 0

    def connect(self, ssid, password):
        self.connections.append((ssid, password))
        if self.connect_errors:
            raise self.connect_errors.pop(0)
        if self.connect_error:
            raise self.connect_error
        self.ssid = ssid
        self.online_at = self.clock.elapsed + self.connect_delay
        self.managed_connecting = True

    def ifconfig(self):
        if not self.isconnected():
            raise AssertionError("An IP must not be reported before connection")
        return ("192.0.2.2", "255.255.255.0", "192.0.2.1", "192.0.2.1")


def load_connect(path, clock, wlan):
    tree = ast.parse((ROOT / path).read_text(), filename=path)
    function = next(node for node in tree.body
                    if isinstance(node, ast.FunctionDef) and node.name == "connect_wifi")
    constants = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            if isinstance(target, ast.Name) and target.id in ("CONNECT_TIMEOUT_S", "CONNECT_RETRY"):
                constants[target.id] = ast.literal_eval(node.value)
    output = []
    namespace = dict(constants, SSID=TARGET, PASSWORD="example-test-password", errno=errno,
                     time=clock, print=lambda *parts: output.append(" ".join(map(str, parts))),
                     network=SimpleNamespace(STA_IF=0, STAT_CONNECTING=1, WLAN=lambda interface: wlan))
    module = ast.Module(body=[function], type_ignores=[])
    exec(compile(module, path, "exec"), namespace)
    return namespace["connect_wifi"], output, constants


class WifiBehaviors:
    def scenarios(self, start=0):
        clock = Clock(start)
        wlan = Wlan(clock)
        connect, output, constants = load_connect(self.EXAMPLE, clock, wlan)
        self.assertEqual(constants, {"CONNECT_TIMEOUT_S": 15, "CONNECT_RETRY": 3})
        yield clock, wlan, connect, output

    def test_online_target_is_reused_without_mutation(self):
        for clock, wlan, connect, _ in self.scenarios():
            wlan.online_at = 0
            self.assertIs(connect(), wlan)
            self.assertEqual(wlan.activations, [])
            self.assertEqual(wlan.connections, [])
            self.assertEqual(wlan.config_queries, ["ssid"])
            self.assertEqual(clock.elapsed, 0)

    def test_online_other_ssid_fails_without_switching_network(self):
        for _, wlan, connect, _ in self.scenarios():
            wlan.online_at = 0
            wlan.ssid = "another-test-network"
            with self.assertRaisesRegex(OSError, "another.*SSID.*Device Core"):
                connect()
            self.assertEqual(wlan.activations, [])
            self.assertEqual(wlan.connections, [])

    def test_unreadable_online_ssid_fails_closed(self):
        for _, wlan, connect, _ in self.scenarios():
            wlan.online_at = 0
            wlan.ssid_error = OSError(errno.EIO, "SSID read failed")
            with self.assertRaises(OSError) as error:
                connect()
            self.assertEqual(error.exception.args[0], errno.EIO)
            self.assertEqual(wlan.connections, [])

    def test_managed_connecting_waits_without_duplicate_connect(self):
        for clock, wlan, connect, _ in self.scenarios():
            wlan.managed_connecting = True
            wlan.online_at = 600
            self.assertIs(connect(), wlan)
            self.assertGreaterEqual(clock.elapsed, 600)
            self.assertEqual(wlan.connections, [])
            self.assertEqual(wlan.activations, [])

    def test_busy_connect_can_finish_in_the_background(self):
        for clock, wlan, connect, _ in self.scenarios():
            wlan.online_at = 400
            wlan.connect_error = OSError(errno.EBUSY)
            self.assertIs(connect(), wlan)
            self.assertTrue(wlan.connections)
            self.assertLessEqual(clock.elapsed, 15000)
            self.assertEqual(wlan.activations, [])

    def test_busy_connect_cannot_accept_a_different_network(self):
        for _, wlan, connect, _ in self.scenarios():
            wlan.online_at = 400
            wlan.ssid = "another-test-network"
            wlan.connect_error = OSError(errno.EBUSY)
            with self.assertRaisesRegex(OSError, "another.*SSID"):
                connect()

    def test_busy_activation_is_waited_and_then_cold_connects(self):
        for clock, wlan, connect, _ in self.scenarios():
            wlan.active_state = False
            wlan.active_errors = [OSError(errno.EBUSY), OSError(errno.EBUSY)]
            self.assertIs(connect(), wlan)
            self.assertGreater(clock.elapsed, 0)
            self.assertEqual(len(wlan.connections), 1)
            self.assertNotIn(False, wlan.activations)

    def test_permanent_busy_is_bounded_across_tick_wrap(self):
        for clock, wlan, connect, _ in self.scenarios(start=65000):
            wlan.connect_error = OSError(errno.EBUSY)
            with self.assertRaisesRegex(OSError, "wifi connect failed.*Device Core"):
                connect()
            self.assertGreaterEqual(clock.elapsed, 45000)
            self.assertLessEqual(clock.elapsed, 45600)
            self.assertTrue(clock.sleeps)
            self.assertEqual(wlan.activations, [])

    def test_managed_connecting_timeout_is_bounded_without_mutation(self):
        for clock, wlan, connect, _ in self.scenarios():
            wlan.managed_connecting = True
            with self.assertRaisesRegex(OSError, "wifi connect failed"):
                connect()
            self.assertLessEqual(clock.elapsed, 45600)
            self.assertEqual(wlan.connections, [])
            self.assertEqual(wlan.activations, [])

    def test_real_connect_error_is_reported_and_retried(self):
        for clock, wlan, connect, output in self.scenarios():
            wlan.connect_errors = [OSError(errno.EIO, "driver failure")]
            self.assertIs(connect(), wlan)
            self.assertEqual(len(wlan.connections), 2)
            self.assertIn(500, clock.sleeps)
            self.assertTrue(any("connect failed:" in line and "driver failure" in line for line in output))

    def test_repeated_real_errors_fail_after_original_retry_count(self):
        for clock, wlan, connect, output in self.scenarios():
            wlan.connect_error = OSError(errno.EIO, "driver failure")
            with self.assertRaisesRegex(OSError, "wifi connect failed"):
                connect()
            self.assertEqual(len(wlan.connections), 3)
            self.assertEqual(sum("connect failed:" in line for line in output), 3)
            self.assertLessEqual(clock.elapsed, 45600)

    def test_cold_connect_activates_once_and_waits_for_ip(self):
        for clock, wlan, connect, _ in self.scenarios():
            wlan.active_state = False
            self.assertIs(connect(), wlan)
            self.assertEqual(wlan.activations, [True])
            self.assertEqual(wlan.connections, [(TARGET, "example-test-password")])
            self.assertGreaterEqual(clock.elapsed, wlan.connect_delay)
            self.assertLessEqual(clock.elapsed, 15000)


class MjpegWifiTests(WifiBehaviors, unittest.TestCase):
    EXAMPLE = EXAMPLES[0]


class CloudAiWifiTests(WifiBehaviors, unittest.TestCase):
    EXAMPLE = EXAMPLES[1]


if __name__ == "__main__":
    unittest.main()

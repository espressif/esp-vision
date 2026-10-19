# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise camera-fixture failure classification without a device or filesystem."""

import importlib.util
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2] / "host/device-core/tests/fixtures/device-checks.py"
SPEC = importlib.util.spec_from_file_location("hardware_fixture", SOURCE)
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)


def sensor_peer(error, operation):
    state = {"size": 320, "format": 2}

    def set_mode(value, key):
        if operation == key and value == 2:
            raise error
        state[key] = value

    return SimpleNamespace(
        RGB565=2, GRAYSCALE=1, QQVGA=2, QVGA=320, VGA=640,
        reset=lambda: None, skip_frames=lambda **kwargs: None,
        set_framesize=lambda value: set_mode(value, "size"),
        set_pixformat=lambda value: set_mode(value, "format"),
        width=lambda: state["size"], height=lambda: state["size"],
        snapshot=lambda: SimpleNamespace(width=lambda: state["size"], height=lambda: state["size"],
                                         size=lambda: state["size"] ** 2 * state["format"]),
    )


class CameraFixtureTests(unittest.TestCase):
    def test_mode_loops_do_not_hide_allocation_timeout_or_unexpected_errors(self):
        for name, operation in (("framesizes", "size"), ("pixformats", "format")):
            for error in (MemoryError("no framebuffer"), OSError("ESP_ERR_TIMEOUT"), ValueError("bad mode")):
                with self.subTest(name=name, error=type(error).__name__), \
                        patch.dict(sys.modules, sensor=sensor_peer(error, operation)), \
                        self.assertRaises(type(error)):
                    getattr(FIXTURE, name)({"framesize": "QVGA"})

    def test_explicit_unsupported_modes_are_reported_with_reason_and_others_still_run(self):
        for name, operation in (("framesizes", "size"), ("pixformats", "format")):
            with self.subTest(name=name), \
                    patch.dict(sys.modules, sensor=sensor_peer(OSError("ESP_ERR_NOT_SUPPORTED"), operation)):
                result = getattr(FIXTURE, name)({"framesize": "QVGA"})
            self.assertFalse(result["results"][0]["supported"])
            self.assertIn("ESP_ERR_NOT_SUPPORTED", result["results"][0]["reason"])
            self.assertTrue(result["results"][1]["supported"])


if __name__ == "__main__":
    unittest.main()

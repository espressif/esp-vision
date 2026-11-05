# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise inference-fixture assertions and resource cleanup, not model accuracy."""
import base64
import hashlib
import contextlib
import io
import json
import runpy
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

FIXTURE = Path(__file__).resolve().parents[2] / "host/device-core/tests/fixtures/device-checks.py"


class EspdlHardwareFixtureTests(unittest.TestCase):
    def setUp(self):
        # runpy's returned dictionary may differ from function.__globals__.
        self.fixture = runpy.run_path(str(FIXTURE))["begin_model"].__globals__
        self.fixture['_fixture_nonce'] = 'fixture-nonce'
        self.mounts = {}
        self.vfs = types.SimpleNamespace(mount=self.mount, umount=self.umount)
        self.modules = patch.dict(sys.modules, {"vfs": self.vfs})
        self.modules.start()
        self.addCleanup(self.modules.stop)

    def mount(self, filesystem=None, path=None, readonly=False):
        if filesystem is None:
            return [(filesystem, path) for path, filesystem in self.mounts.items()]
        if path in self.mounts:
            raise OSError("mount already exists")
        self.assertTrue(readonly)
        self.mounts[path] = filesystem

    def umount(self, filesystem):
        for path, current in list(self.mounts.items()):
            if current is filesystem:
                del self.mounts[path]
                return
        raise OSError(22)

    def upload(self, data=b"verified model"):
        self.fixture["begin_model"]("fixture-nonce", len(data), "/evhil_test")
        self.fixture["append_model"](0, base64.b64encode(data))
        return hashlib.sha256(data).hexdigest()

    def test_ram_model_read_seek_and_idempotent_cleanup(self):
        data = b"verified model"
        self.fixture["mount_model"](self.upload(data))
        filesystem = self.mounts["/evhil_test"]
        with filesystem.open("/model.espdl", "rb") as stream:
            self.assertEqual(stream.seek(0, 2), len(data))
            self.assertEqual(stream.seek(0), 0)
            self.assertEqual(stream.read(), data)
        with self.assertRaises(OSError):
            filesystem.open("/model.espdl", "wb")
        self.fixture["close_model"]()
        self.fixture["close_model"]()
        self.assertFalse(self.mounts)
        self.assertIsNone(self.fixture["_model_bytes"])

    def test_incomplete_or_corrupted_model_never_mounts(self):
        self.fixture["begin_model"]("fixture-nonce", 100, "/evhil_test")
        with self.assertRaisesRegex(AssertionError, "incomplete"):
            self.fixture["mount_model"]("0" * 64)
        self.fixture["close_model"]()
        self.upload()
        with self.assertRaisesRegex(AssertionError, "SHA-256"):
            self.fixture["mount_model"]("0" * 64)
        self.fixture["close_model"]()
        self.assertFalse(self.mounts)

    def test_append_commit_interruption_and_lost_ack_are_idempotent(self):
        self.fixture["begin_model"]("fixture-nonce", 6, "/evhil_test")

        class InterruptedBytes(bytearray):
            interrupt = True

            def __setitem__(buffer, key, value):
                super().__setitem__(key, value)
                if buffer.interrupt:
                    buffer.interrupt = False
                    raise KeyboardInterrupt()

        self.fixture["_model_bytes"] = InterruptedBytes(6)
        encoded = base64.b64encode(b"abc").decode()
        with self.assertRaises(KeyboardInterrupt):
            self.fixture["append_model"](0, encoded)
        self.assertEqual(self.fixture["_model_offset"], 0)
        with patch("builtins.print", side_effect=TimeoutError("ACK lost")):
            with self.assertRaises(TimeoutError):
                self.fixture["model_command"]("append", 0, encoded)
        self.assertEqual(self.fixture["_model_offset"], 3)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.fixture["model_command"]("append", 0, encoded)
        self.assertEqual(json.loads(output.getvalue().removeprefix("__EVHILACK__")),
                         ["fixture-nonce", "append", dict(offset=0, end=3, bytes=3, current=3)])
        self.assertEqual(self.fixture["_model_bytes"], b"abc\0\0\0")

    def test_append_rejects_conflicts_gaps_old_replay_and_overflow(self):
        self.fixture["begin_model"]("fixture-nonce", 6, "/evhil_test")
        append = lambda offset, data: self.fixture["append_model"](offset, base64.b64encode(data))
        append(0, b"abc")
        for offset, data in [(0, b"xyz"), (1, b"bcd"), (4, b"xy"), (3, b"toolong"), (-1, b"x"), (3, b"")]:
            with self.subTest(offset=offset, data=data), self.assertRaises(AssertionError):
                append(offset, data)
        append(3, b"def")
        with self.assertRaisesRegex(AssertionError, "out of order"):
            append(0, b"abc")
        self.assertEqual(self.fixture["_model_bytes"], b"abcdef")

    def test_mount_collision_preserves_the_existing_filesystem(self):
        previous = object()
        self.mounts["/evhil_test"] = previous
        with self.assertRaisesRegex(OSError, "already exists"):
            self.fixture["mount_model"](self.upload())
        self.fixture["close_model"]()
        self.assertIs(self.mounts["/evhil_test"], previous)

    def test_failed_unmount_keeps_model_reachable_for_cleanup_retry(self):
        self.fixture["mount_model"](self.upload())
        original = self.vfs.umount
        self.vfs.umount = Mock(side_effect=OSError("busy"))
        with self.assertRaisesRegex(OSError, "busy"):
            self.fixture["close_model"]()
        self.assertIsNotNone(self.fixture["_model_vfs"])
        self.assertIsNotNone(self.fixture["_model_bytes"])
        self.vfs.umount = original
        self.fixture["close_model"]()
        self.assertFalse(self.mounts)

    def test_cleanup_ack_requires_real_success_and_empty_model_before_begin(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.fixture['model_command']('close')
        self.assertEqual(json.loads(output.getvalue().removeprefix('__EVHILACK__')),
                         ['fixture-nonce', 'close', dict(bytes=0, offset=0, mounted=False)])
        self.fixture['mount_model'](self.upload())
        original = self.vfs.umount
        self.vfs.umount = Mock(side_effect=OSError('busy'))
        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaises(OSError):
            self.fixture['model_command']('close')
        self.assertEqual(output.getvalue(), '')
        self.assertIsNotNone(self.fixture['_model_bytes'])
        self.vfs.umount = original
        with contextlib.redirect_stdout(output):
            self.fixture['model_command']('close')
        self.assertIsNone(self.fixture['_model_bytes'])
        self.assertFalse(self.mounts)
        self.assertEqual(json.loads(output.getvalue().removeprefix('__EVHILACK__'))[2]['mounted'], False)

    def test_uncertain_mount_result_is_cleaned_by_object_identity(self):
        expected = self.upload()
        original = self.vfs.mount

        def interrupted_mount(*args, **kwargs):
            result = original(*args, **kwargs)
            if args:
                raise KeyboardInterrupt()
            return result

        self.vfs.mount = interrupted_mount
        with self.assertRaises(KeyboardInterrupt):
            self.fixture["mount_model"](expected)
        self.fixture["close_model"]()
        self.assertFalse(self.mounts)
        self.assertIsNone(self.fixture["_model_bytes"])

    def test_model_load_and_inference_failure_always_release_camera_and_detector(self):
        frame = Mock()
        frame.width.return_value, frame.height.return_value = 320, 240
        frame.copy.return_value = frame
        for stage in ("load", "shape", "bounds", "score", "determinism", "ok"):
            with self.subTest(stage=stage):
                sensor = types.SimpleNamespace(RGB565=1, QVGA=2, reset=Mock(), set_pixformat=Mock(),
                                               set_framesize=Mock(), skip_frames=Mock(), snapshot=Mock(return_value=frame),
                                               shutdown=Mock())
                detector = Mock()
                outputs = {"shape": [(1, 2)], "bounds": [(400, 2, 3, 4, .5, 0)],
                           "score": [(1, 2, 3, 4, 2, 0)], "ok": [(1, 2, 3, 4, .5, 0)]}
                detector.detect.return_value = outputs.get(stage, [])
                if stage == "determinism":
                    detector.detect.side_effect = [[], [(1, 2, 3, 4, .5, 0)]]
                constructor = Mock(return_value=detector)
                if stage == "load":
                    constructor.side_effect = MemoryError("arena unavailable")
                timer = Mock(side_effect=range(100))
                timing = types.SimpleNamespace(ticks_ms=timer, ticks_us=timer, ticks_diff=lambda end, start: end - start)
                with patch.dict(sys.modules, {"sensor": sensor, "espdl": types.SimpleNamespace(ESPDet=constructor), "time": timing}):
                    if stage == "ok":
                        result = self.fixture["espdl_inference"]({"framesize": "QVGA", "modelPath": "/ram/model.espdl", "frames": 2})
                        self.assertEqual(result["frames"], 2)
                    else:
                        with self.assertRaises((AssertionError, MemoryError)):
                            self.fixture["espdl_inference"]({"framesize": "QVGA", "modelPath": "/ram/model.espdl", "frames": 2})
                sensor.shutdown.assert_called_once()
                if stage != "load":
                    detector.deinit.assert_called_once()


if __name__ == "__main__":
    unittest.main()

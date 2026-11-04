# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Retained EV-MUX previews need valid wire framing and strict JPEG entropy."""
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from preview_tcp import encode_frame
import json
import zlib
from test_uvc import validate_evmux_preview


def jpeg():
    image = Image.new('RGB', (320, 240))
    image.putdata([((x * 13 + y * 3) & 255, (x * 7 + y * 19) & 255,
                    ((x ^ y) * 11) & 255) for y in range(240) for x in range(320)])
    stream = io.BytesIO()
    image.save(stream, 'JPEG', quality=60)
    return stream.getvalue()


def wire(payload, **overrides):
    metadata = {'channel': 'preview.frame', 'contentType': 'image/jpeg',
                'width': 320, 'height': 240, **overrides}
    return encode_frame(metadata, payload)


class RetainedEvmuxPreview(unittest.TestCase):
    def validate(self, data, **options):
        with tempfile.TemporaryDirectory() as directory:
            recording = Path(directory) / 'wire.bin'
            recording.write_bytes(data)
            return validate_evmux_preview(recording, **options)

    def test_every_preview_is_strictly_decoded_without_changing_its_bytes(self):
        payload = jpeg()
        frames = self.validate(wire(payload) * 3)
        self.assertEqual([frame.payload for frame in frames], [payload] * 3)

    def test_valid_crc_does_not_hide_loss_of_entropy_in_middle_preview(self):
        payload = jpeg()
        sos = payload.index(b'\xff\xda')
        scan = sos + 2 + int.from_bytes(payload[sos + 2:sos + 4], 'big')
        damaged = payload[:scan + 10] + payload[scan + 520:]
        with Image.open(io.BytesIO(damaged)) as image:
            image.load()
        # Re-encode the wire CRC around the malformed JPEG: framing is valid.
        with self.assertRaisesRegex(ValueError, 'Strict FFmpeg JPEG validation failed'):
            self.validate(wire(payload) + wire(damaged) + wire(payload))

    def test_crc_corruption_still_fails_before_image_acceptance(self):
        payload = jpeg()
        broken = bytearray(wire(payload))
        broken[-10] ^= 1
        with self.assertRaises(AssertionError):
            self.validate(wire(payload) * 3 + broken)

    def test_incomplete_final_wire_frame_is_not_discarded(self):
        payload = jpeg()
        with self.assertRaisesRegex(AssertionError, 'Incomplete EV-MUX recording'):
            self.validate(wire(payload) * 3 + wire(payload)[:-7])

    def test_metadata_must_describe_qvga_jpeg(self):
        payload = jpeg()
        for metadata in ({'width': 160}, {'contentType': 'application/octet-stream'}):
            with self.subTest(metadata=metadata), self.assertRaises(AssertionError):
                self.validate(wire(payload) * 2 + wire(payload, **metadata))

    def test_missing_strict_decoder_dependency_cannot_pass(self):
        payload = jpeg()
        with patch('uvc_frames.shutil.which', return_value=None):
            with self.assertRaisesRegex(RuntimeError, 'requires ffmpeg'):
                self.validate(wire(payload) * 3)

    def test_preview_without_wire_crc_is_rejected(self):
        payload = jpeg()
        meta = {'channel': 'preview.frame', 'contentType': 'image/jpeg', 'width': 320, 'height': 240}
        with self.assertRaisesRegex(AssertionError, 'nonzero and exact'):
            self.validate(wire(payload) * 2 + encode_frame(meta, payload, crc=False))

    def test_explicit_recorder_close_allows_only_provably_nonpreview_header(self):
        payload = jpeg()
        release = encode_frame({'channel': 'user.rpc', 'type': 'rsp', 'method': 'session.release'}, b'{"ok":true}')
        tail = b'\x1eEVMUX/1 h=117 p=104 c=34C872E1\r\n'
        raw = wire(payload) * 3 + release + tail
        with self.assertRaisesRegex(AssertionError, 'Incomplete EV-MUX recording'):
            self.validate(raw)
        report = {}
        self.assertEqual(len(self.validate(raw, allow_collector_tail=True, tail_report=report)), 3)
        self.assertTrue(report['allowed'])
        self.assertEqual(report['bytes'], len(tail))
        self.assertEqual(report['retainedBodyBytes'], 0)

    def test_collector_exemption_never_discards_preview_or_unknown_body(self):
        payload = jpeg()
        release = encode_frame({'channel': 'user.rpc', 'type': 'rsp', 'method': 'session.release'}, b'{"ok":true}')
        prefix = wire(payload) * 3 + release
        production_meta = {'sid': 'preview', 'seq': 0, 'channel': 'preview.frame', 'type': 'data',
                           'encoding': 'binary', 'route': 'usb', 'contentType': 'image/jpeg',
                           'ts_ms': 0, 'width': 320, 'height': 240}
        full = encode_frame(production_meta, payload)
        end = full.index(b'\r\n') + 2
        for bad in (full[:end], full[:end + 10], full[:-7],
                    b'\x1eEVMUX/1 h=117 p=104 c=34C872E1\r\n{'):
            with self.subTest(tailBytes=len(bad)), self.assertRaises(AssertionError):
                self.validate(prefix + bad, allow_collector_tail=True)
        with self.assertRaisesRegex(AssertionError, 'not after release ACK'):
            self.validate(wire(payload) * 3 + b'\x1eEVMUX/1 h=117 p=104 c=34C872E1\r\n', allow_collector_tail=True)

    def test_webrepl_compatibility_entry_stops_without_importing_network(self):
        source = Path(__file__).resolve().parents[2] / 'example/07-Network/00-WebREPL/webrepl.py'
        code = compile(source.read_text(), str(source), 'exec')
        with patch('builtins.__import__', side_effect=AssertionError('Network service imports forbidden')):
            with self.assertRaisesRegex(RuntimeError, 'use Device Core over TCP'):
                exec(code, {})


if __name__ == '__main__':
    unittest.main()

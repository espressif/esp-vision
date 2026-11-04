# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Independent JPEG fixtures detect incomplete UVC payloads in retained streams."""

import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

from PIL import Image, ImageFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from uvc_frames import decode_mjpeg_frames


def jpeg(color, size=(320, 240)):
    output = io.BytesIO()
    Image.new('RGB', size, color).save(output, 'JPEG', quality=60)
    return output.getvalue()


def patterned_jpeg():
    image = Image.new('RGB', (320, 240))
    image.putdata([((x * 13 + y * 3) & 255, (x * 7 + y * 19) & 255,
                    ((x ^ y) * 11) & 255) for y in range(240) for x in range(320)])
    output = io.BytesIO()
    image.save(output, 'JPEG', quality=60)
    return output.getvalue()


class UvcCompleteFrames(unittest.TestCase):
    def test_all_frames_decode_and_preserve_original_bytes(self):
        frames = [jpeg('red'), jpeg('blue')]
        self.assertEqual(decode_mjpeg_frames(b''.join(frames), 2), frames)

    def test_missing_middle_usb_payload_is_rejected_despite_soi_and_eoi(self):
        frame = jpeg('green')
        damaged = frame[:510] + frame[1020:]
        self.assertTrue(damaged.startswith(b'\xff\xd8') and damaged.endswith(b'\xff\xd9'))
        with self.assertRaises((ValueError, OSError, SyntaxError)):
            decode_mjpeg_frames(jpeg('red') + damaged + jpeg('blue'), 3)

    def test_partial_final_frame_is_rejected(self):
        with self.assertRaises(ValueError):
            decode_mjpeg_frames(jpeg('red') + jpeg('blue')[:-2], 2)

    def test_missing_whole_frame_is_rejected(self):
        with self.assertRaises(ValueError):
            decode_mjpeg_frames(jpeg('red'), 2)

    def test_unframed_bytes_are_rejected(self):
        for data in [b'partial' + jpeg('red'), jpeg('red') + b'partial']:
            with self.subTest(prefix=data[:8]), self.assertRaises(ValueError):
                decode_mjpeg_frames(data, 1)

    def test_wrong_dimensions_are_rejected(self):
        with self.assertRaises(ValueError):
            decode_mjpeg_frames(jpeg('red', (160, 120)), 1)

    def test_permissive_global_decoder_setting_is_rejected(self):
        previous = ImageFile.LOAD_TRUNCATED_IMAGES
        try:
            ImageFile.LOAD_TRUNCATED_IMAGES = True
            with self.assertRaises(ValueError):
                decode_mjpeg_frames(jpeg('red'), 1)
        finally:
            ImageFile.LOAD_TRUNCATED_IMAGES = previous

    def test_entropy_payload_loss_is_rejected_even_when_pillow_loads(self):
        frame = patterned_jpeg()
        sos = frame.index(b'\xff\xda')
        scan = sos + 2 + int.from_bytes(frame[sos + 2:sos + 4], 'big')
        self.assertEqual(decode_mjpeg_frames(frame, 1), [frame])
        for delta in (10, 520, 1030):
            with self.subTest(scan_offset=delta):
                offset = scan + delta
                damaged = frame[:offset] + frame[offset + 510:]
                self.assertTrue(damaged.startswith(b'\xff\xd8') and damaged.endswith(b'\xff\xd9'))
                with Image.open(io.BytesIO(damaged)) as image:
                    image.load()
                    self.assertEqual(image.size, (320, 240))
                with self.assertRaisesRegex(ValueError, 'Strict FFmpeg JPEG validation failed'):
                    decode_mjpeg_frames(jpeg('red') + damaged + jpeg('blue'), 3)

    def test_missing_ffmpeg_is_an_explicit_failure(self):
        with patch('uvc_frames.shutil.which', return_value=None):
            with self.assertRaisesRegex(RuntimeError, 'requires ffmpeg on PATH'):
                decode_mjpeg_frames(jpeg('red'), 1)

    def test_empty_stream_cannot_be_accepted_with_zero_expected_frames(self):
        with self.assertRaisesRegex(ValueError, 'positive expected frame count'):
            decode_mjpeg_frames(b'', 0)


if __name__ == '__main__':
    unittest.main()

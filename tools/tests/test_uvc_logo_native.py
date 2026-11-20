# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import ctypes
import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from PIL import Image, JpegImagePlugin


ROOT = Path(__file__).resolve().parents[2]


class UvcLogoTest(unittest.TestCase):
    def test_generated_logo_has_requested_dimensions_and_bounded_size(self):
        cc = shutil.which('cc')
        if cc is None:
            self.skipTest('native C compiler is unavailable')
        with tempfile.TemporaryDirectory(prefix='ev-uvc-logo-') as temp:
            library = Path(temp) / 'logo.so'
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                            str(ROOT / 'platform' / 'uvc_logo.c'), '-o', str(library)], check=True)
            create = ctypes.CDLL(str(library)).esp_vision_uvc_logo_create
            create.argtypes = (ctypes.c_uint16, ctypes.c_uint16, ctypes.POINTER(ctypes.c_size_t))
            create.restype = ctypes.c_void_p
            free = ctypes.CDLL(None).free
            free.argtypes = (ctypes.c_void_p,)
            free.restype = None

            for width, height in ((240, 240), (320, 240), (480, 480), (640, 480),
                                  (800, 480), (1024, 600), (1280, 720), (319, 241)):
                with self.subTest(width=width, height=height):
                    size = ctypes.c_size_t()
                    jpeg = create(width, height, ctypes.byref(size))
                    self.assertTrue(jpeg)
                    try:
                        self.assertLess(size.value, 16 * 1024)
                        data = ctypes.string_at(jpeg, size.value)
                        with Image.open(io.BytesIO(data)) as img:
                            img.load()
                            self.assertEqual(img.size, (width, height))
                            self.assertEqual(img.mode, 'RGB')
                            self.assertEqual(JpegImagePlugin.get_sampling(img), 1)
                            self.assertEqual(img.getextrema(), ((32, 224),) * 3)
                    finally:
                        free(jpeg)

            size = ctypes.c_size_t(123)
            self.assertIsNone(create(0, 240, ctypes.byref(size)))
            self.assertEqual(size.value, 0)
            self.assertIsNone(create(65535, 65535, ctypes.byref(size)))
            self.assertEqual(size.value, 0)

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise the production PNG file paths with codecs enabled and disabled."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_jpeg_validation import function

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MP_ERROR_TEXT(s) (s)
#define IMLIB_IMAGE_MAX_SIZE(s) (s)
#define PIXFORMAT_PNG 1
#define FA_READ 1
#define FA_OPEN_EXISTING 2
#define FA_WRITE 4
#define FA_CREATE_ALWAYS 8
typedef struct { int open; size_t pos; } file_t;
typedef struct { uint32_t png_w, png_h, png_size; } png_read_settings_t;
typedef struct { uint32_t w, h, size; int pixfmt; uint8_t *pixels; } image_t;
static const int mp_type_RuntimeError = 1;
static jmp_buf error;
static const char *message;
static int opens, closes, reads, allocations, compressions;
static uint8_t input[] = {
    0x89, 0x50, 0x4e, 0x47, 13, 10, 26, 10, 0, 0, 0, 13,
    'I', 'H', 'D', 'R', 0, 0, 0, 128, 0, 0, 0, 64
};
static uint8_t output[64] = "existing";
static size_t output_size = 8;
static void file_open(file_t *fp, const char *path, bool buffered, int flags) {
    (void) path; (void) buffered;
    fp->open = 1; fp->pos = 0; opens++;
    if (flags & FA_CREATE_ALWAYS) output_size = 0;
}
static void file_close(file_t *fp) { assert(fp->open); fp->open = 0; closes++; }
static void file_raise_error(file_t *fp, const char *msg) {
    file_close(fp); message = msg; longjmp(error, 1);
}
static void file_raise_corrupted(file_t *fp) { file_raise_error(fp, "corrupt"); }
static void mp_raise_msg_varg(const int *type, const char *msg) {
    assert(type == &mp_type_RuntimeError); message = msg; longjmp(error, 1);
}
static void file_seek(file_t *fp, size_t pos) { fp->pos = pos; }
static void file_read(file_t *fp, void *data, size_t size) {
    assert(fp->open && fp->pos + size <= sizeof(input));
    memcpy(data, input + fp->pos, size); fp->pos += size; reads++;
}
static size_t file_size(file_t *fp) { assert(fp->open); return sizeof(input); }
static void image_alloc(image_t *img, size_t size) {
    allocations++; img->pixels = malloc(size); assert(img->pixels);
}
static void file_write(file_t *fp, const void *data, size_t size) {
    assert(fp->open && size <= sizeof(output));
    memcpy(output, data, size); output_size = size;
}
static bool png_compress(image_t *src, image_t *dst) {
    (void) src; compressions++; dst->pixels = input; dst->size = sizeof(input); return false;
}
static void fb_free(void) {}
'''

CHECKS = r'''
int main(void) {
    image_t img = {0};
    file_t fp;
    png_read_settings_t rs;
    file_open(&fp, "asset", false, FA_READ);
#if defined(IMLIB_ENABLE_PNG_DECODER)
    png_read_geometry(&fp, &img, "asset", &rs);
    assert(img.w == 128 && img.h == 64 && img.pixfmt == PIXFORMAT_PNG);
    file_close(&fp);
    img = (image_t){0};
    png_read(&img, "asset");
    assert(img.size == sizeof(input) && !memcmp(img.pixels, input, sizeof(input)));
    free(img.pixels);
#else
    if (!setjmp(error)) {
        png_read_geometry(&fp, &img, "asset", &rs);
        assert(0);
    }
    assert(!strcmp(message, "PNG decoder is not enabled"));
    assert(!fp.open && reads == 0 && allocations == 0);
    if (!setjmp(error)) {
        png_read(&img, "asset");
        assert(0);
    }
    assert(!strcmp(message, "PNG decoder is not enabled"));
    assert(opens == closes && reads == 0 && allocations == 0);
#endif
    img = (image_t){ .pixfmt = 2 };
#if defined(IMLIB_ENABLE_PNG_ENCODER)
    png_write(&img, "output.png");
    assert(compressions == 1 && output_size == sizeof(input));
#else
    int old_opens = opens;
    if (!setjmp(error)) {
        png_write(&img, "output.png");
        assert(0);
    }
    assert(!strcmp(message, "PNG encoder is not enabled"));
    assert(opens == old_opens && output_size == 8 && !memcmp(output, "existing", 8));
#endif
    img = (image_t){ .pixfmt = PIXFORMAT_PNG, .pixels = input, .size = sizeof(input) };
    png_write(&img, "output.png");
    assert(output_size == sizeof(input) && !memcmp(output, input, sizeof(input)));
    assert(opens == closes);
    return 0;
}
'''


class PngAvailabilityTests(unittest.TestCase):
    def test_file_paths_with_codecs_disabled_and_enabled(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler unavailable')
        source = (ROOT / 'components/imlib/upstream/png.c').read_text()
        functions = '\n'.join(function(source, signature) for signature in (
            'void png_read_geometry(', 'void png_read_pixels(', 'void png_read(', 'void png_write(',
        ))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(HARNESS + functions + CHECKS)
            for decoder, encoder in ((False, False), (False, True), (True, False), (True, True)):
                with self.subTest(decoder=decoder, encoder=encoder):
                    flags = []
                    if decoder:
                        flags.append('-DIMLIB_ENABLE_PNG_DECODER')
                    if encoder:
                        flags.append('-DIMLIB_ENABLE_PNG_ENCODER')
                    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter',
                                    *flags, str(path / 'test.c'), '-o', str(path / 'test')],
                                   check=True, capture_output=True, text=True)
                    subprocess.run([str(path / 'test')], check=True, capture_output=True, text=True)

    def test_python_file_operations_use_exception_safe_framebuffer_scope(self):
        source = (ROOT / 'modules/py_image.c').read_text()
        for signature, operation in (
            ('mp_obj_t py_image_make_new(', 'imlib_read_geometry('),
            ('static mp_obj_t py_image_save(', 'imlib_save_image('),
        ):
            body = function(source, signature)
            self.assertLess(body.index('py_image_alloc_cleanup_begin(&cleanup, false)'), body.index(operation))
            self.assertLess(body.index(operation), body.index('nlr_pop_jump_callback(true)'))


if __name__ == '__main__':
    unittest.main()

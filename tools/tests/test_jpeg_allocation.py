# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Fault-inject IDF allocations while running production JPEG buffer management."""
import ctypes
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_jpeg_validation import function

ROOT = Path(__file__).resolve().parents[2]


class JpegAllocationRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler unavailable')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        source = (ROOT / 'platform/jpeg.c').read_text()
        production = '\n'.join(function(source, signature) for signature in (
            'static void esp_vision_jpeg_free_cpu_buffer(',
            'static size_t esp_vision_jpeg_capacity(',
            'static void esp_vision_jpeg_free_buffers(',
            'static esp_err_t esp_vision_jpeg_ensure_buffers('))
        prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 6
#define JPEG_ENC_ALLOC_INPUT_BUFFER 0
#define JPEG_ENC_ALLOC_OUTPUT_BUFFER 1
typedef struct {int buffer_direction;} jpeg_encode_memory_alloc_cfg_t;
static struct {uint8_t *input_buf,*jpeg_buf,*cpu_buf;size_t input_buf_size,jpeg_buf_size,cpu_buf_size;} s_jpeg;
static size_t allocation_calls, fail_call;
static void *jpeg_alloc_encoder_mem(size_t size,const jpeg_encode_memory_alloc_cfg_t *cfg,size_t *allocated_size)
{
    assert(cfg->buffer_direction==JPEG_ENC_ALLOC_INPUT_BUFFER || cfg->buffer_direction==JPEG_ENC_ALLOC_OUTPUT_BUFFER);
    allocation_calls++;
    size=(size+31u)&~(size_t)31u;
    // Matches the documented actual IDF ordering: size is assigned even if the
    // following allocation fails. No JPEG/cache-management logic is mocked.
    *allocated_size=size;
    return allocation_calls==fail_call ? NULL : calloc(1,size);
}
'''
        wrappers = r'''
void allocation_reset(void) {esp_vision_jpeg_free_buffers();allocation_calls=0;fail_call=0;}
void allocation_fail_at(size_t value) {fail_call=value;}
int allocation_ensure(size_t raw_size) {return esp_vision_jpeg_ensure_buffers(raw_size);}
size_t allocation_count(void) {return allocation_calls;}
size_t allocation_capacity(int output) {return output ? s_jpeg.jpeg_buf_size : s_jpeg.input_buf_size;}
int allocation_present(int output) {return (output ? s_jpeg.jpeg_buf : s_jpeg.input_buf)!=NULL;}
void allocation_poison_legacy_state(void) {esp_vision_jpeg_free_buffers();s_jpeg.input_buf_size=768000;s_jpeg.jpeg_buf_size=1156096;}
'''
        (directory / 'allocation.c').write_text(prelude + production + wrappers)
        result = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                                 str(directory / 'allocation.c'), '-o', str(directory / 'allocation.so')],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        cls.library = ctypes.CDLL(str(directory / 'allocation.so'))
        cls.library.allocation_ensure.argtypes = [ctypes.c_size_t]
        cls.library.allocation_ensure.restype = ctypes.c_int
        cls.library.allocation_fail_at.argtypes = [ctypes.c_size_t]
        cls.library.allocation_count.restype = ctypes.c_size_t
        cls.library.allocation_capacity.argtypes = [ctypes.c_int]
        cls.library.allocation_capacity.restype = ctypes.c_size_t
        cls.library.allocation_present.argtypes = [ctypes.c_int]
        cls.library.allocation_present.restype = ctypes.c_int
        cls.addClassCleanup(cls.library.allocation_reset)

    def setUp(self):
        self.library.allocation_reset()

    def assert_buffer(self, output, present, minimum=0):
        self.assertEqual(bool(self.library.allocation_present(output)), present)
        capacity = self.library.allocation_capacity(output)
        if present:
            self.assertGreaterEqual(capacity, minimum)
        else:
            self.assertEqual(capacity, 0, 'NULL buffers must never retain a positive cached capacity')

    def test_input_failure_clears_capacity_and_same_size_retries(self):
        self.library.allocation_fail_at(1)
        self.assertEqual(self.library.allocation_ensure(153600), 6)
        self.assert_buffer(0, False)
        self.assert_buffer(1, False)
        self.library.allocation_fail_at(0)
        self.assertEqual(self.library.allocation_ensure(153600), 0)
        self.assertEqual(self.library.allocation_count(), 3)
        self.assert_buffer(0, True, 153600)
        self.assert_buffer(1, True, 234496)

    def test_output_oom_then_smaller_frame_retries_instead_of_passing_null(self):
        self.assertEqual(self.library.allocation_ensure(153600), 0)
        self.library.allocation_fail_at(4)
        self.assertEqual(self.library.allocation_ensure(768000), 6)
        self.assert_buffer(0, True, 768000)
        self.assert_buffer(1, False)
        self.library.allocation_fail_at(0)
        self.assertEqual(self.library.allocation_ensure(153600), 0)
        self.assertEqual(self.library.allocation_count(), 5)
        self.assert_buffer(0, True, 768000)
        self.assert_buffer(1, True, 234496)

    def test_input_growth_oom_then_smaller_frame_preserves_other_buffer(self):
        self.assertEqual(self.library.allocation_ensure(153600), 0)
        self.library.allocation_fail_at(3)
        self.assertEqual(self.library.allocation_ensure(768000), 6)
        self.assert_buffer(0, False)
        self.assert_buffer(1, True, 234496)
        self.library.allocation_fail_at(0)
        self.assertEqual(self.library.allocation_ensure(76800), 0)
        self.assertEqual(self.library.allocation_count(), 4)
        self.assert_buffer(0, True, 76800)

    def test_null_pointer_forces_recovery_even_from_legacy_poisoned_capacities(self):
        self.library.allocation_poison_legacy_state()
        self.assertEqual(self.library.allocation_ensure(153600), 0)
        self.assertEqual(self.library.allocation_count(), 2)
        self.assert_buffer(0, True, 153600)
        self.assert_buffer(1, True, 234496)

    def test_healthy_cached_buffers_do_not_allocate_on_repeated_hot_path(self):
        self.assertEqual(self.library.allocation_ensure(153600), 0)
        for raw_size in (153600, 76800, 153600, 76800):
            self.assertEqual(self.library.allocation_ensure(raw_size), 0)
        self.assertEqual(self.library.allocation_count(), 2)

    def test_repeated_failures_remain_retryable_and_deinit_clears_all_state(self):
        for attempt in (1, 2, 3):
            self.library.allocation_fail_at(attempt)
            self.assertEqual(self.library.allocation_ensure(153600), 6)
            self.assert_buffer(0, False)
        self.library.allocation_fail_at(0)
        self.assertEqual(self.library.allocation_ensure(153600), 0)
        self.library.allocation_reset()
        self.assert_buffer(0, False)
        self.assert_buffer(1, False)


if __name__ == '__main__':
    unittest.main()

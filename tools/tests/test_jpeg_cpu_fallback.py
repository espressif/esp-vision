# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Run production JPEG allocation/fallback paths with the real imlib encoder."""
import ctypes
import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_jpeg_validation import function

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).with_name('jpeg_cpu_stubs')

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "imlib.h"
#include "jpeg_validate.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_INVALID_RESPONSE 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_INVALID_STATE 5
#define ESP_ERR_NO_MEM 6
#define ESP_ERR_TIMEOUT 7
#define MALLOC_CAP_8BIT 1
#define JPEG_ENC_ALLOC_INPUT_BUFFER 0
#define JPEG_ENC_ALLOC_OUTPUT_BUFFER 1
#define ESP_VISION_HAS_NEW_JPEG_ENCODER 0
#define JPEG_ENCODE_IN_FORMAT_RGB565 2
#define JPEG_ENCODE_IN_FORMAT_GRAY 1
#define JPEG_DOWN_SAMPLING_YUV422 2
#define JPEG_DOWN_SAMPLING_GRAY 1
typedef int jpeg_enc_input_format_t,jpeg_down_sampling_type_t;
typedef struct {int buffer_direction;} jpeg_encode_memory_alloc_cfg_t;
typedef struct {uint32_t width,height;int src_type,sub_sample,image_quality;} jpeg_encode_cfg_t;
static struct {void *encoder;uint8_t *input_buf,*jpeg_buf,*cpu_buf;size_t input_buf_size,jpeg_buf_size,cpu_buf_size;} s_jpeg;
static size_t idf_calls,scratch_calls,cpu_calls,hardware_calls,live_bytes,peak_bytes,shrink_calls,engine_calls;
static int engine_result;
static bool s_jpeg_hw_unavailable;
static size_t native_largest=2u<<20,scratch_capacity;
static size_t requested[64];
static uint64_t fail_mask;
static int scratch_fail,cpu_fault,hardware_result,vm_task=1,shrink_mode,single_block;
static uintptr_t old_scratch,new_cache;
static void test_log(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGD(...) test_log(__VA_ARGS__)
static size_t heap_caps_get_largest_free_block(uint32_t caps) {assert(caps==MALLOC_CAP_8BIT);return native_largest;}
static size_t heap_caps_get_free_size(uint32_t caps) {assert(caps==MALLOC_CAP_8BIT);return 2u<<20;}
static const uint8_t *source_pixels;
static uint8_t *result_pixels;
static size_t result_size;
typedef struct {void *raw;uint8_t *data;size_t size;} allocation_t;
static allocation_t allocations[32];
static void checked_guards(const allocation_t *allocation) {
    for(size_t k=0;k<16;k++) {
        assert(allocation->data[-16+(int)k]==0xa7);
        assert(allocation->data[allocation->size+k]==0xb9);
    }
}
static void *tracked_alloc(size_t size) {
    assert(!single_block || live_bytes==0);
    for(size_t i=0;i<32;i++) if(!allocations[i].raw) {
        uint8_t *raw=malloc(size+192);assert(raw);
        uint8_t *data=(uint8_t *)(((uintptr_t)raw+127)&~(uintptr_t)63);
        memset(data-16,0xa7,16);memset(data+size,0xb9,16);
        allocations[i]=(allocation_t){raw,data,size};live_bytes+=size;
        if(live_bytes>peak_bytes)peak_bytes=live_bytes;
        return data;
    }
    abort();
}
static void tracked_free(void *data) {
    if(!data)return;
    for(size_t i=0;i<32;i++) if(allocations[i].data==data) {
        checked_guards(&allocations[i]);
        live_bytes-=allocations[i].size;free(allocations[i].raw);
        allocations[i]=(allocation_t){0};return;
    }
    abort();
}
static void *jpeg_alloc_encoder_mem(size_t size,const jpeg_encode_memory_alloc_cfg_t *cfg,size_t *actual) {
    assert(!s_jpeg.cpu_buf && !s_jpeg.cpu_buf_size);
    assert(cfg->buffer_direction==0 || cfg->buffer_direction==1);
    idf_calls++;assert(idf_calls<64);requested[idf_calls]=size;
    *actual=(size+63)&~(size_t)63;
    return (fail_mask & ((uint64_t)1<<idf_calls)) ? NULL : tracked_alloc(*actual);
}
static void *esp_vision_memory_malloc(size_t size,uint32_t caps) {
    assert(size>=1024 && size<=JPEG_MAX_ALLOC_SIZE && size<=native_largest-64 && caps==MALLOC_CAP_8BIT);
    assert(!s_jpeg.input_buf && !s_jpeg.jpeg_buf && !s_jpeg.cpu_buf);
    assert(!s_jpeg.input_buf_size && !s_jpeg.jpeg_buf_size && !s_jpeg.cpu_buf_size);
    scratch_capacity=size;scratch_calls++;return scratch_fail ? NULL : tracked_alloc(size);
}
static void *esp_vision_memory_realloc(void *data,size_t size,uint32_t caps) {
    assert(data && size && size<=scratch_capacity && caps==MALLOC_CAP_8BIT);
    assert(!s_jpeg.cpu_buf && !s_jpeg.cpu_buf_size);
    old_scratch=(uintptr_t)data;shrink_calls++;
    if(shrink_mode==2)return NULL;
    for(size_t i=0;i<32;i++) if(allocations[i].data==data) {
        checked_guards(&allocations[i]);assert(size<=allocations[i].size);
        if(shrink_mode==1) {
            // Model a generic heap move to non-DMA-aligned CPU memory.
            assert(!single_block);
            size_t old_size=allocations[i].size;
            uint8_t *moved=tracked_alloc(size+1);
            memcpy(moved+1,data,size);
            tracked_free(data);
            for(size_t j=0;j<32;j++) if(allocations[j].data==moved) {
                memset(moved-15,0xa7,16);
                allocations[j].data=moved+1;allocations[j].size=size;
                live_bytes--;new_cache=(uintptr_t)(moved+1);
                assert(old_size>=size);return moved+1;
            }
            abort();
        }
        live_bytes-=allocations[i].size-size;allocations[i].size=size;
        memset(allocations[i].data+size,0xb9,16);
        new_cache=(uintptr_t)data;return data;
    }
    abort();
}
static bool esp_vision_camera_is_vm_task(void) {return vm_task;}
static int esp_vision_jpeg_ensure_encoder(void) {engine_calls++;s_jpeg.encoder=(void *)(uintptr_t)1;return engine_result;}
static int jpeg_del_encoder_engine(void *engine) {assert(engine);return ESP_OK;}
extern bool imlib_jpeg_compress(image_t *,image_t *,int,bool,jpeg_subsampling_t);
static int jpeg_encoder_process(void *engine,const jpeg_encode_cfg_t *cfg,const uint8_t *input,
                                uint32_t input_size,uint8_t *output,uint32_t capacity,uint32_t *length) {
    assert(engine && cfg && input==s_jpeg.input_buf && output==s_jpeg.jpeg_buf && capacity==s_jpeg.jpeg_buf_size);
    assert(memcmp(input,source_pixels,input_size)==0);
    assert(!s_jpeg.cpu_buf && !s_jpeg.cpu_buf_size);
    assert(((uintptr_t)input%64)==0 && ((uintptr_t)output%64)==0);
    hardware_calls++;*length=0;
    if(hardware_result==ESP_OK) {
        image_t src={.w=(int)cfg->width,.h=(int)cfg->height,
                     .pixfmt=cfg->src_type==JPEG_ENCODE_IN_FORMAT_RGB565 ? PIXFORMAT_RGB565 : PIXFORMAT_GRAYSCALE,
                     .pixels=(uint8_t *)input};
        image_t dst={.w=src.w,.h=src.h,.pixfmt=PIXFORMAT_JPEG,.size=capacity,.pixels=output};
        assert(!imlib_jpeg_compress(&src,&dst,cfg->image_quality,false,JPEG_SUBSAMPLING_AUTO));
        *length=dst.size;
    }
    return hardware_result;
}
static bool intercepted_jpeg_compress(image_t *src,image_t *dst,int quality,bool grow,jpeg_subsampling_t sampling) {
    assert(dst->pixels && dst->size==scratch_capacity && !grow && sampling==JPEG_SUBSAMPLING_AUTO);
    cpu_calls++;
    if(cpu_fault==1)return true;
    bool overflow=imlib_jpeg_compress(src,dst,quality,grow,sampling);
    if(cpu_fault==2 && dst->size)dst->pixels[dst->size-1]=0;
    if(cpu_fault==3)dst->size=0;
    if(cpu_fault==4)dst->size=scratch_capacity+1;
    if(cpu_fault==5)dst->pixels++;
    return overflow;
}
#define free tracked_free
#define jpeg_compress intercepted_jpeg_compress
'''

WRAPPERS = r'''
void fallback_reset(void) {
    esp_vision_jpeg_deinit();assert(live_bytes==0);
    idf_calls=scratch_calls=cpu_calls=hardware_calls=peak_bytes=shrink_calls=engine_calls=0;engine_result=ESP_OK;
    memset(requested,0,sizeof(requested));fail_mask=0;scratch_fail=cpu_fault=0;vm_task=1;
    hardware_result=ESP_ERR_TIMEOUT;result_pixels=NULL;result_size=0;
    native_largest=2u<<20;scratch_capacity=0;
    shrink_mode=single_block=0;old_scratch=new_cache=0;
}
void fallback_engine(int result) {engine_result=result;}
void fallback_shrink(int mode,int only_one_block) {shrink_mode=mode;single_block=only_one_block;}
void fallback_largest(size_t value) {native_largest=value;}
void fallback_config(uint64_t failures,int scratch,int fault,int hardware,int vm) {
    fail_mask=failures;scratch_fail=scratch;cpu_fault=fault;hardware_result=hardware;vm_task=vm;
}
int fallback_encode(uint8_t *pixels,int width,int height,int color,int quality) {
    source_pixels=pixels;
    image_t image={.w=width,.h=height,.pixfmt=color ? PIXFORMAT_RGB565 : PIXFORMAT_GRAYSCALE,
                   .size=(uint32_t)(width*height*(color ? 2 : 1)),.pixels=pixels};
    result_pixels=(uint8_t *)(uintptr_t)1;result_size=123;
    return esp_vision_jpeg_encode(&image,quality,&result_pixels,&result_size);
}
void *fallback_output(void) {return result_pixels;}
size_t fallback_value(int field) {
    switch(field) {
      case 0:return result_size;case 1:return idf_calls;case 2:return scratch_calls;
      case 3:return cpu_calls;case 4:return hardware_calls;case 5:return live_bytes;
      case 6:return peak_bytes;case 7:return s_jpeg.input_buf_size;case 8:return s_jpeg.jpeg_buf_size;
      case 9:return scratch_capacity;
      case 10:return s_jpeg.cpu_buf_size;case 11:return shrink_calls;
      case 12:return old_scratch;case 13:return new_cache;case 14:return engine_calls;
      default:abort();
    }
}
size_t fallback_requested(int call) {assert(call>0 && call<64);return requested[call];}
'''


class JpegCpuFallbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler unavailable')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        source = (ROOT / 'platform/jpeg.c').read_text()
        production = '\n'.join(function(source, signature) for signature in (
            'static void esp_vision_jpeg_free_cpu_buffer(',
            'static void esp_vision_jpeg_free_buffers(',
            'void esp_vision_jpeg_deinit(',
            'static size_t esp_vision_jpeg_capacity(',
            'static esp_err_t esp_vision_jpeg_ensure_buffers(',
            'static esp_err_t esp_vision_jpeg_image_format(',
            'static esp_err_t esp_vision_jpeg_encode_cpu(',
            'esp_err_t esp_vision_jpeg_encode('))
        harness = cls.path / 'fallback.c'
        harness.write_text(HARNESS + production + WRAPPERS)
        # The real codec retains its original file/header; host shims only make
        # RGB565/GRAY and forbidden MicroPython allocation entry points linkable.
        codec = cls.path / 'jpege.c'
        codec_source = (ROOT / 'components/imlib/upstream/jpege.c').read_text()
        end = '#endif // (OMV_JPEG_CODEC_ENABLE == 0)'
        codec.write_text(codec_source[:codec_source.index(end) + len(end)] + '\n')
        command = [compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-sign-compare',
                   '-shared', '-fPIC', '-I', str(FIXTURES), '-I', str(ROOT / 'platform')]
        result = subprocess.run([*command, '-Djpeg_compress=imlib_jpeg_compress', '-c',
                                 str(codec), '-o', str(cls.path / 'codec.o')], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        result = subprocess.run([*command, str(harness), str(cls.path / 'codec.o'),
                                 str(ROOT / 'platform/jpeg_validate.c'), '-lm',
                                 '-o', str(cls.path / 'fallback.so')], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        cls.counter = 0

    def setUp(self):
        type(self).counter += 1
        fresh = self.path / f'fresh-{self.counter}.so'
        shutil.copy2(self.path / 'fallback.so', fresh)
        self.lib = ctypes.CDLL(str(fresh))
        self.lib.fallback_config.argtypes = [ctypes.c_uint64, *([ctypes.c_int] * 4)]
        self.lib.fallback_encode.argtypes = [ctypes.c_void_p, *([ctypes.c_int] * 4)]
        self.lib.fallback_output.restype = ctypes.c_void_p
        self.lib.fallback_value.argtypes = [ctypes.c_int]
        self.lib.fallback_value.restype = ctypes.c_size_t
        self.lib.fallback_requested.argtypes = [ctypes.c_int]
        self.lib.fallback_requested.restype = ctypes.c_size_t
        self.lib.fallback_largest.argtypes = [ctypes.c_size_t]
        self.lib.fallback_engine.argtypes = [ctypes.c_int]
        self.lib.fallback_shrink.argtypes = [ctypes.c_int, ctypes.c_int]
        self.lib.fallback_reset()
        self.addCleanup(self.lib.fallback_reset)

    def encode(self, width=800, height=480, color=True, quality=60, data=None):
        raw = data if data is not None else bytes([0x35, 0x8a] if color else [71]) * (width * height)
        buffer = ctypes.create_string_buffer(raw)
        result = self.lib.fallback_encode(buffer, width, height, color, quality)
        self.assertEqual(buffer.raw[:-1], raw, 'CPU encoding must preserve every source byte')
        pointer = self.lib.fallback_output()
        length = self.lib.fallback_value(0)
        return result, ctypes.string_at(pointer, length) if pointer and length else b''

    def assert_empty(self):
        self.assertFalse(self.lib.fallback_output())
        self.assertEqual(self.lib.fallback_value(0), 0)

    def check_decoded(self, jpeg, size, color):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow required for independent full JPEG decoding')
        with Image.open(io.BytesIO(jpeg)) as decoded:
            decoded.load()
            self.assertEqual(decoded.size, size)
            self.assertEqual(decoded.mode, 'RGB' if color else 'L')

    def test_real_rgb_and_gray_cpu_results_decode_and_use_exact_cache(self):
        for color in (True, False):
            self.lib.fallback_reset()
            self.lib.fallback_config(1 << 1, 0, 0, 7, 1)
            result, jpeg = self.encode(color=color)
            self.assertEqual(result, 0)
            self.check_decoded(jpeg, (800, 480), color)
            self.assertEqual(self.lib.fallback_value(4), 0)
            self.assertEqual(self.lib.fallback_value(7), 0)
            self.assertEqual(self.lib.fallback_value(1), 1, 'CPU result must not use a second IDF allocation')
            self.assertEqual(self.lib.fallback_value(8), 0, 'CPU cache must remain separate from DMA output')
            self.assertEqual(self.lib.fallback_value(10), len(jpeg))
            self.assertEqual(self.lib.fallback_value(5), len(jpeg))
            self.assertEqual(self.lib.fallback_value(11), 1)
            self.assertLessEqual(self.lib.fallback_value(6), 1 << 20)

    def test_engine_oom_uses_validated_cpu_without_retry_until_reset(self):
        self.lib.fallback_engine(6)
        for _ in range(3):
            result, jpeg = self.encode(64, 48)
            self.assertEqual(result, 0)
            self.check_decoded(jpeg, (64, 48), True)
        self.assertEqual(self.lib.fallback_value(14), 1)
        self.assertEqual(self.lib.fallback_value(1), 0)
        self.assertEqual(self.lib.fallback_value(4), 0)
        self.lib.fallback_reset()
        self.lib.fallback_config(0, 0, 0, 0, 1)
        result, jpeg = self.encode(64, 48)
        self.assertEqual(result, 0)
        self.check_decoded(jpeg, (64, 48), True)
        self.assertEqual(self.lib.fallback_value(14), 1)
        self.assertEqual(self.lib.fallback_value(4), 1)

    def test_fresh_quality100_initializes_real_quantization_tables(self):
        self.lib.fallback_config(1 << 1, 0, 0, 7, 1)
        result, jpeg = self.encode(64, 48, quality=100)
        self.assertEqual(result, 0)
        self.check_decoded(jpeg, (64, 48), True)

    def test_hardware_output_oom_frees_partial_input_before_cpu_scratch(self):
        self.lib.fallback_config(1 << 2, 0, 0, 7, 1)
        result, jpeg = self.encode()
        self.assertEqual(result, 0)
        self.assertTrue(jpeg)
        self.assertEqual(self.lib.fallback_value(4), 0)
        self.assertEqual(self.lib.fallback_value(7), 0)

    def test_scratch_or_shrink_failure_clears_output_and_frees_original_once(self):
        for scratch, shrink in ((1, 0), (0, 2)):
            self.lib.fallback_reset()
            self.lib.fallback_config(1 << 1, scratch, 0, 7, 1)
            self.lib.fallback_shrink(shrink, 0)
            self.assertEqual(self.encode()[0], 6)
            self.assert_empty()
            self.assertEqual(self.lib.fallback_value(5), 0)
            self.assertEqual(self.lib.fallback_value(8), 0)
            self.assertEqual(self.lib.fallback_value(10), 0)

    def test_single_native_block_shrinks_in_place_without_another_allocation(self):
        self.lib.fallback_largest(655360)
        self.lib.fallback_config((1 << 64) - 1, 0, 0, 7, 1)
        self.lib.fallback_shrink(0, 1)
        result, jpeg = self.encode()
        self.assertEqual(result, 0)
        self.check_decoded(jpeg, (800, 480), True)
        self.assertEqual(self.lib.fallback_value(1), 1)
        self.assertEqual(self.lib.fallback_value(6), 655296)
        self.assertEqual(self.lib.fallback_value(5), len(jpeg))
        self.assertEqual(self.lib.fallback_output(), self.lib.fallback_value(12))
        self.assertEqual(self.lib.fallback_output(), self.lib.fallback_value(13))

    def test_shrink_move_keeps_new_unaligned_pointer_as_cpu_only_cache(self):
        self.lib.fallback_config(1 << 1, 0, 0, 7, 1)
        self.lib.fallback_shrink(1, 0)
        result, jpeg = self.encode(64, 48, quality=100)
        self.assertEqual(result, 0)
        self.check_decoded(jpeg, (64, 48), True)
        self.assertNotEqual(self.lib.fallback_output(), self.lib.fallback_value(12))
        self.assertEqual(self.lib.fallback_output(), self.lib.fallback_value(13))
        self.assertEqual(self.lib.fallback_output() % 64, 1)
        self.assertEqual(self.lib.fallback_value(5), len(jpeg))
        self.assertEqual(self.lib.fallback_value(8), 0)

    def test_repeated_cpu_encode_releases_previous_cache_before_next_scratch(self):
        self.lib.fallback_config((1 << 64) - 1, 0, 0, 7, 1)
        self.lib.fallback_shrink(0, 1)
        for quality in (60, 100, 60):
            result, jpeg = self.encode(64, 48, quality=quality)
            self.assertEqual(result, 0)
            self.check_decoded(jpeg, (64, 48), True)
            self.assertEqual(self.lib.fallback_value(5), len(jpeg))
            self.assertEqual(self.lib.fallback_value(10), len(jpeg))
        self.assertEqual(self.lib.fallback_value(3), 3)
        self.assertEqual(self.lib.fallback_value(11), 3)

    def test_shrink_failure_can_retry_without_poisoned_cache(self):
        self.lib.fallback_config((1 << 64) - 1, 0, 0, 7, 1)
        self.lib.fallback_shrink(2, 1)
        self.assertEqual(self.encode()[0], 6)
        self.assert_empty()
        self.assertEqual(self.lib.fallback_value(5), 0)
        self.lib.fallback_shrink(0, 1)
        result, jpeg = self.encode()
        self.assertEqual(result, 0)
        self.check_decoded(jpeg, (800, 480), True)
        self.assertEqual(self.lib.fallback_value(5), len(jpeg))

    def test_overflow_and_malformed_codec_results_never_escape(self):
        for fault, expected in ((1, 6), (2, 3), (3, 2), (4, 2), (5, 2)):
            self.lib.fallback_reset()
            self.lib.fallback_config(1 << 1, 0, fault, 7, 1)
            self.assertEqual(self.encode()[0], expected)
            self.assert_empty()
            self.assertEqual(self.lib.fallback_value(5), 0)

    def test_real_high_entropy_overflow_is_explicit_no_mem(self):
        state = 0x214c94
        data = bytearray()
        for _ in range(800 * 480 * 2):
            state ^= (state << 13) & 0xffffffff
            state ^= state >> 17
            state ^= (state << 5) & 0xffffffff
            data.append(state & 255)
        for largest in (2 << 20, 655360):
            with self.subTest(largest=largest):
                self.lib.fallback_reset()
                self.lib.fallback_largest(largest)
                self.lib.fallback_config(1 << 1, 0, 0, 7, 1)
                self.assertEqual(self.encode(quality=100, data=bytes(data))[0], 6)
                self.assert_empty()
                self.assertEqual(self.lib.fallback_value(5), 0)

    def test_fragmented_655360_byte_native_block_encodes_real_lcd_frame(self):
        self.lib.fallback_largest(655360)
        self.lib.fallback_config(1 << 1, 0, 0, 7, 1)
        result, jpeg = self.encode()
        self.assertEqual(result, 0)
        self.assertEqual(self.lib.fallback_value(9), 655296)
        self.check_decoded(jpeg, (800, 480), True)
        self.assertEqual(self.lib.fallback_value(4), 0)
        self.assertLessEqual(self.lib.fallback_value(6), 655296)

    def test_tiny_native_blocks_fail_without_underflow_or_codec_calls(self):
        for largest in (0, 32, 64, 64 + 1023):
            with self.subTest(largest=largest):
                self.lib.fallback_reset()
                self.lib.fallback_largest(largest)
                self.lib.fallback_config(1 << 1, 0, 0, 7, 1)
                self.assertEqual(self.encode()[0], 6)
                self.assert_empty()
                self.assertEqual(self.lib.fallback_value(2), 0)
                self.assertEqual(self.lib.fallback_value(3), 0)

    def test_worker_cannot_enter_shared_imlib_fallback(self):
        self.lib.fallback_config(1 << 1, 0, 0, 7, 0)
        self.assertEqual(self.encode()[0], 6)
        self.assert_empty()
        self.assertEqual(self.lib.fallback_value(2), 0)
        self.assertEqual(self.lib.fallback_value(3), 0)

    def test_hardware_process_failure_never_falls_back(self):
        self.lib.fallback_config(0, 0, 0, 7, 1)
        self.assertEqual(self.encode(64, 48)[0], 7)
        self.assert_empty()
        self.assertEqual(self.lib.fallback_value(4), 1)
        self.assertEqual(self.lib.fallback_value(2), 0)
        self.assertEqual(self.lib.fallback_value(3), 0)

    def test_cpu_cache_can_transition_back_to_aligned_hardware_buffers(self):
        for move in (0, 1):
            with self.subTest(move=move):
                self.lib.fallback_reset()
                self.lib.fallback_config(1 << 1, 0, 0, 7, 1)
                self.lib.fallback_shrink(move, 0)
                self.assertEqual(self.encode()[0], 0)
                calls = self.lib.fallback_value(1)
                cpu_calls = self.lib.fallback_value(3)
                self.lib.fallback_config(0, 0, 0, 0, 1)
                result, jpeg = self.encode(320, 240)
                self.assertEqual(result, 0)
                self.check_decoded(jpeg, (320, 240), True)
                self.assertEqual(self.lib.fallback_value(1), calls + 2)
                self.assertEqual(self.lib.fallback_value(4), 1)
                self.assertEqual(self.lib.fallback_value(3), cpu_calls)
                self.assertGreaterEqual(self.lib.fallback_value(7), 320 * 240 * 2)
                self.assertGreaterEqual(self.lib.fallback_value(8), 320 * 240 * 3 + 4096)
                self.assertEqual(self.lib.fallback_value(10), 0)
                self.assertEqual(self.lib.fallback_value(5), self.lib.fallback_value(7) + self.lib.fallback_value(8))
                # Larger capture exhausts hardware input; CPU may re-enter
                # after freeing both hardware caches without retaining either.
                self.lib.fallback_config(1 << (calls + 3), 0, 0, 7, 1)
                result, jpeg = self.encode()
                self.assertEqual(result, 0)
                self.check_decoded(jpeg, (800, 480), True)
                self.assertEqual(self.lib.fallback_value(3), cpu_calls + 1)
                self.assertEqual(self.lib.fallback_value(7), 0)
                self.assertEqual(self.lib.fallback_value(8), 0)
                self.assertEqual(self.lib.fallback_value(5), len(jpeg))


if __name__ == '__main__':
    unittest.main()

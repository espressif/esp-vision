# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile real JPEG guards and fault-inject the production encoder entry points."""
import base64
import ctypes
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Self-generated pixel patterns using Pillow 10.2.0, quality=83. L 1x1
# baseline (pixel 80); RGB 4x3 progressive (linear x/y channel patterns).
# Test execution needs neither Pillow nor a decoder. Entropy validity remains
# outside this structural check; complete decoding is a separate hardware check.
BASELINE = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAUEBAUEAwUFBAUGBgUGCA4JCAcHCBEMDQoOFBEVFBMRExMWGB8bFhceFxMTGyUc"
    "HiAhIyMjFRomKSYiKR8iIyL/wAALCAABAAEBAREA/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgED"
    "AwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RF"
    "RkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJ"
    "ytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/9oACAEBAAA/APMq/9k="
)

PROGRESSIVE = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAUEBAUEAwUFBAUGBgUGCA4JCAcHCBEMDQoOFBEVFBMRExMWGB8bFhceFxMTGyUc"
    "HiAhIyMjFRomKSYiKR8iIyL/2wBDAQYGBggHCBAJCRAiFhMWIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIi"
    "IiIiIiIiIiIiIiIiIiL/wgARCAADAAQDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAb/xAAVAQEBAAAAAAAAAAAA"
    "AAAAAAADBf/aAAwDAQACEAMQAAABohNf/8QAFRABAQAAAAAAAAAAAAAAAAAABBT/2gAIAQEAAQUCAE0X/8QAGhEAAQUBAAAA"
    "AAAAAAAAAAAAAwACBBMhQf/aAAgBAwEBPwGMUlLd4v/EABYRAQEBAAAAAAAAAAAAAAAAAAEAAv/aAAgBAgEBPwF03//EABYQ"
    "AQEBAAAAAAAAAAAAAAAAAAIAI//aAAgBAQAGPwI4m//EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAT8hRf/aAAwDAQAC"
    "AAMAAAAQD//EABgRAAIDAAAAAAAAAAAAAAAAAAAxAUFR/9oACAEDAQE/EGmlzh//xAAWEQADAAAAAAAAAAAAAAAAAAAAETH/"
    "2gAIAQIBAT8QZp//xAAYEAACAwAAAAAAAAAAAAAAAAAAAVFhwf/aAAgBAQABPxDIjhWf/9k="
)

def function(source, signature, *, last=False):
    start = source.rindex(signature) if last else source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(signature)


def header_segments(data):
    position = 2
    while position < len(data):
        start = position
        assert data[position] == 255
        marker = data[position + 1]
        length = int.from_bytes(data[position + 2:position + 4], 'big')
        end = position + 2 + length
        yield marker, start, position + 4, end
        position = end
        if marker == 0xDA:
            return


class JpegValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler unavailable')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        library = Path(cls.directory.name) / 'jpeg.so'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(ROOT / 'platform/jpeg_validate.c'), '-o', str(library)], check=True,
                       capture_output=True, text=True)
        cls.library = ctypes.CDLL(str(library))
        cls.validate = cls.library.esp_vision_jpeg_validate
        cls.validate.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                 ctypes.c_uint32, ctypes.c_uint32]
        cls.validate.restype = ctypes.c_bool

    def accepts(self, data, width=1, height=1):
        buffer = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        return bool(self.validate(buffer, len(data), width, height))

    def test_real_baseline_progressive_and_expected_dimensions(self):
        self.assertTrue(self.accepts(BASELINE))
        self.assertTrue(self.accepts(PROGRESSIVE, 4, 3))
        self.assertFalse(self.accepts(BASELINE, 2, 1))
        self.assertFalse(self.accepts(BASELINE, 1, 0))
        self.assertFalse(self.accepts(BASELINE, 1 << 16, 1))
        self.assertFalse(self.validate(None, 100, 1, 1))
        self.assertFalse(self.accepts(b'\xff\xd8\xff\xd9'))

    def test_every_truncated_prefix_and_trailing_bytes_are_rejected(self):
        for data, width, height in ((BASELINE, 1, 1), (PROGRESSIVE, 4, 3)):
            for end in range(len(data)):
                with self.subTest(width=width, end=end):
                    self.assertFalse(self.accepts(data[:end], width, height))
            for suffix in (b'\0', b'\xff', b'\xff\xd9', data):
                self.assertFalse(self.accepts(data + suffix, width, height))

    def test_quantizer_and_huffman_shape_and_references(self):
        segments = list(header_segments(BASELINE))
        for marker, start, body, end in segments:
            if marker in (0xDB, 0xC4):
                self.assertFalse(self.accepts(BASELINE[:start] + BASELINE[end:]))
            if marker == 0xDB:
                zero = bytearray(BASELINE); zero[body + 1] = 0
                self.assertFalse(self.accepts(zero))
                # A 16-bit table is bounded syntactically but cannot supply an
                # 8-bit frame's referenced quantizer in this output contract.
                table = b'\x10' + b'\x00\x01' * 64
                expanded = BASELINE[:start] + b'\xff\xdb' + (len(table) + 2).to_bytes(2, 'big') + table + BASELINE[end:]
                self.assertFalse(self.accepts(expanded))
            if marker == 0xC4:
                for counts, symbols in ((bytes([3]) + bytes(15), bytes(3)),
                                        (bytes([2]) + bytes(15), bytes(2)),
                                        (bytes(16), b'')):
                    table = BASELINE[body:body + 1] + counts + symbols
                    invalid = BASELINE[:start] + b'\xff\xc4' + (len(table) + 2).to_bytes(2, 'big') + table + BASELINE[end:]
                    self.assertFalse(self.accepts(invalid))
        sof = next(item for item in segments if item[0] == 0xC0)
        bad = bytearray(BASELINE); bad[sof[2] + 8] = 3
        self.assertFalse(self.accepts(bad))
        sos = next(item for item in segments if item[0] == 0xDA)
        bad = bytearray(BASELINE); bad[sos[2] + 2] = 0x33
        self.assertFalse(self.accepts(bad))

    def test_frame_component_scan_shape_and_initial_coverage(self):
        for data, width, height in ((BASELINE, 1, 1), (PROGRESSIVE, 4, 3)):
            segments = list(header_segments(data))
            sof = next(item for item in segments if item[0] in (0xC0, 0xC2))
            sos = next(item for item in segments if item[0] == 0xDA)
            for offset, value in ((sof[2], 12), (sof[2] + 5, 0), (sof[2] + 7, 0),
                                  (sos[2], 0), (sos[2] + 1, 99)):
                bad = bytearray(data); bad[offset] = value
                self.assertFalse(self.accepts(bad, width, height))
            self.assertFalse(self.accepts(data[:sof[3]] + b'\xff\xd9', width, height))
            self.assertFalse(self.accepts(data[:sos[3]] + b'\xff\xd9', width, height))
            self.assertFalse(self.accepts(data[:sos[1]] + data[sof[1]:sof[3]] + data[sos[1]:], width, height))
        segments = list(header_segments(PROGRESSIVE))
        sof = next(item for item in segments if item[0] == 0xC2)
        bad = bytearray(PROGRESSIVE); bad[sof[2] + 9] = bad[sof[2] + 6]
        self.assertFalse(self.accepts(bad, 4, 3))
        sos = next(item for item in segments if item[0] == 0xDA)
        # Remove the last component from the sole initial DC scan. Refinement
        # scans cannot stand in for that component's missing initial scan.
        body = bytearray(PROGRESSIVE[sos[2]:sos[3]])
        count = body[0]; self.assertGreater(count, 1)
        del body[1 + (2 * (count - 1)):1 + (2 * count)]
        body[0] -= 1
        bad = PROGRESSIVE[:sos[1]] + b'\xff\xda' + (len(body) + 2).to_bytes(2, 'big') + body + PROGRESSIVE[sos[3]:]
        self.assertFalse(self.accepts(bad, 4, 3))

    def test_scan_stuffing_restart_boundaries_and_fill(self):
        sos = next(item for item in header_segments(BASELINE) if item[0] == 0xDA)
        header = BASELINE[:sos[3]]
        entropy = BASELINE[sos[3]:-2]
        eoi = b'\xff\xd9'
        self.assertTrue(self.accepts(header + entropy + b'\xff\x00' + eoi))
        self.assertTrue(self.accepts(BASELINE[:2] + b'\xff' + BASELINE[2:]))
        # Fill FF bytes are valid before markers, not before stuffed zero.
        self.assertFalse(self.accepts(header + entropy + b'\xff\xff\x00' + eoi))
        with_dri = BASELINE[:sos[1]] + b'\xff\xdd\x00\x04\x00\x01' + BASELINE[sos[1]:sos[3]]
        self.assertTrue(self.accepts(with_dri + entropy + b'\xff\xd0' + entropy + eoi))
        self.assertFalse(self.accepts(with_dri + b'\xff\xd0' + entropy + eoi))
        self.assertFalse(self.accepts(with_dri + entropy + b'\xff\xd0' + eoi))
        self.assertFalse(self.accepts(with_dri + entropy + b'\xff\xd1' + entropy + eoi))
        self.assertFalse(self.accepts(header + entropy + b'\xff\xd0' + entropy + eoi))


class JpegEncoderOutputTests(unittest.TestCase):
    def compile_run(self, source):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler unavailable')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'encoder.c'
            binary = Path(directory) / 'encoder'
            path.write_text(source)
            result = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(ROOT / 'platform'), str(path),
                                     str(ROOT / 'platform/jpeg_validate.c'), '-o', str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_encoder_success_is_validated_before_any_image_route_can_send(self):
        source = (ROOT / 'platform/jpeg.c').read_text()
        control = (ROOT / 'platform/ev_control_transport.c').read_text()
        preview = function((ROOT / 'platform/preview.c').read_text(), 'esp_err_t esp_vision_preview_flush(')
        handlers = '\n'.join(function(control, signature) for signature in (
            'static esp_err_t ev_control_send_debug_capture_frame(',
            'static esp_err_t ev_control_send_debug_capture_screen('))
        common = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include "jpeg_validate.h"
typedef int esp_err_t;
typedef struct {int32_t w,h;uint32_t pixfmt,size;uint8_t *pixels;} image_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_INVALID_RESPONSE 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_INVALID_STATE 5
#define ESP_ERR_NO_MEM 6
#define ESP_ERR_TIMEOUT 7
#define PIXFORMAT_GRAYSCALE 1
#define PIXFORMAT_RGB565 2
#define PIXFORMAT_JPEG 5
static uint8_t input_pixels[1]={80},encoder_input_pixels[1],encoded[2048];
static image_t test_image={.w=1,.h=1,.pixfmt=PIXFORMAT_GRAYSCALE,.pixels=input_pixels};
static int codec_result,ensure_result,returned_size,corruption;
static int opened,closed;
static void fill_encoded(uint8_t *output) {
    memcpy(output,valid_jpeg,sizeof(valid_jpeg));
    if(corruption==1) output[0]=0;
    if(corruption==2) output[sizeof(valid_jpeg)-1]=0;
    if(corruption==3) {
        for(size_t i=0;i+8<sizeof(valid_jpeg);i++) {
            if(output[i]==255 && output[i+1]==192){output[i+8]=2;break;}
        }
    }
}
'''
        fixture = 'static const uint8_t valid_jpeg[]={' + ','.join(str(byte) for byte in BASELINE) + '};\n'
        # Place the fixture after includes and before the fake driver writes it.
        common = common.replace('static uint8_t input_pixels', fixture + 'static uint8_t input_pixels')
        route_stubs = r'''
#define ESP_VISION_JPEG_QUALITY_LOW 60
#define ESP_VISION_PREVIEW_USJ_MIN_INTERVAL_MS 100
#define EV_STREAM_USER 1
#define ESP_RETURN_ON_ERROR(call,tag,text) do { int err=(call);(void)(tag);(void)(text);if(err!=ESP_OK)return err; } while(0)
static const char *TAG="jpeg-test";
static uint32_t s_last_usj_flush_ms;
static int queued,errors;
static bool esp_vision_uvc_preview_ready(const image_t *image) {(void)image;return true;}
static bool ev_mux_preview_observers_ready(void) {return true;}
static int esp_vision_uvc_publish(const uint8_t *data,size_t length,bool fixed,bool *accepted) {
    assert(data==encoded && length==sizeof(valid_jpeg) && !fixed);*accepted=true;return ESP_OK;
}
static bool esp_vision_preview_is_ready(void) {return true;}
static const char *ev_channel_get(int stream) {assert(stream==EV_STREAM_USER);return "uart";}
static uint64_t esp_timer_get_time(void) {return 1000000;}
static bool ev_mux_preview_available(int stream) {assert(stream==EV_STREAM_USER);return true;}
static void ev_mux_record_tx_drop(void) {}
static int esp_vision_preview_write_frame(const image_t *image,const uint8_t *data,size_t length,bool owner,bool observers) {
    assert(owner && observers);
    assert(image==&test_image && data==encoded && length==sizeof(valid_jpeg));queued++;return 0;
}
static uint32_t ev_control_json_get_u32(const char *request,const char *key,uint32_t fallback) {
    (void)request;(void)key;return fallback;
}
static int esp_vision_camera_get_observed_frame(image_t *image) {*image=test_image;return ESP_OK;}
static int esp_vision_display_get_frame(image_t *image) {*image=test_image;return ESP_OK;}
static const char *esp_err_to_name(int result) {(void)result;return "mock";}
static int ev_control_send_error(const char *method,uint32_t seq,const char *code,const char *message) {
    assert(method && seq==7 && strcmp(code,"JPEG_FAILED")==0 && message);errors++;return -1;
}
static int ev_control_send_binary_rsp(const char *method,const char *mime,const void *pixels,
                                      size_t length,uint32_t seq,int32_t width,int32_t height) {
    assert(method && strcmp(mime,"image/jpeg")==0 && pixels==encoded && length==sizeof(valid_jpeg));
    assert(seq==7 && width==1 && height==1);queued++;return ESP_OK;
}
'''
        main = r'''
static void rejected(int expected) {
    uint8_t *data=(uint8_t *)(uintptr_t)1;size_t size=123;
    assert(esp_vision_jpeg_encode(&test_image,60,&data,&size)==expected);
    assert(data==NULL && size==0);
    int before=queued,prior_errors=errors;
    assert(esp_vision_preview_flush(&test_image)==expected && queued==before);
    assert(ev_control_send_debug_capture_frame(7,"{}")==-1 && queued==before);
    assert(ev_control_send_debug_capture_screen(7,"{}")==-1 && queued==before);
    assert(errors==prior_errors+2);
}
int main(void) {
    uint8_t *data=(uint8_t *)(uintptr_t)1;size_t size=123;
    image_t compressed={.w=1,.h=1,.pixfmt=PIXFORMAT_JPEG,.size=sizeof(valid_jpeg),.pixels=(uint8_t *)valid_jpeg};
    assert(esp_vision_jpeg_encode(&compressed,60,&data,&size)==ESP_OK);
    assert(data==valid_jpeg && size==sizeof(valid_jpeg));
    for(size_t end=0;end<sizeof(valid_jpeg);end++) {
        compressed.size=end;data=(void *)(uintptr_t)1;size=123;
        assert(esp_vision_jpeg_encode(&compressed,60,&data,&size)==ESP_ERR_INVALID_RESPONSE);
        assert(data==NULL && size==0);
    }
    compressed.size=sizeof(valid_jpeg);compressed.w=2;
    assert(esp_vision_jpeg_encode(&compressed,60,&data,&size)==ESP_ERR_INVALID_RESPONSE);
    assert(data==NULL && size==0);
    returned_size=(int)sizeof(valid_jpeg);
    assert(esp_vision_jpeg_encode(&test_image,60,&data,&size)==ESP_OK);
    assert(data==encoded && size==sizeof(valid_jpeg));
    assert(esp_vision_preview_flush(&test_image)==ESP_OK && queued==1);
    assert(ev_control_send_debug_capture_frame(7,"{}")==ESP_OK && queued==2);
    assert(ev_control_send_debug_capture_screen(7,"{}")==ESP_OK && queued==3);
    returned_size=0;rejected(ESP_ERR_INVALID_SIZE);
    returned_size=-1;rejected(ESP_ERR_INVALID_SIZE);
    returned_size=(int)sizeof(encoded)+1;rejected(ESP_ERR_INVALID_SIZE);
    returned_size=(int)sizeof(valid_jpeg);
    for(corruption=1;corruption<=3;corruption++)rejected(ESP_ERR_INVALID_RESPONSE);
    corruption=0;codec_result=ESP_ERR_INVALID_STATE;rejected(ESP_ERR_INVALID_STATE);
    codec_result=0;ensure_result=ESP_ERR_NO_MEM;rejected(ESP_ERR_NO_MEM);
    ensure_result=0;
#if SOC_JPEG_ENCODE_SUPPORTED
    s_jpeg_hw_unavailable=false;
#endif
    data=(uint8_t *)(uintptr_t)1;size=123;
    assert(esp_vision_jpeg_encode(NULL,60,&data,&size)==ESP_ERR_INVALID_ARG && data==NULL && size==0);
    data=(uint8_t *)(uintptr_t)1;size=123;
    assert(esp_vision_jpeg_encode(&test_image,0,&data,&size)==ESP_ERR_INVALID_ARG && data==NULL && size==0);
    data=(uint8_t *)(uintptr_t)1;size=123;
    assert(esp_vision_jpeg_encode(&test_image,60,NULL,&size)==ESP_ERR_INVALID_ARG && size==0);
    assert(esp_vision_jpeg_encode(&test_image,60,&data,NULL)==ESP_ERR_INVALID_ARG && data==NULL);
    assert(opened==closed);
    return 0;
}
'''
        hardware = r'''
typedef int jpeg_enc_input_format_t,jpeg_down_sampling_type_t;
#define JPEG_ENCODE_IN_FORMAT_RGB565 2
#define JPEG_ENCODE_IN_FORMAT_GRAY 1
#define JPEG_DOWN_SAMPLING_YUV422 2
#define JPEG_DOWN_SAMPLING_GRAY 1
typedef struct {uint32_t width,height;int src_type,sub_sample,image_quality;} jpeg_encode_cfg_t;
#define SOC_JPEG_ENCODE_SUPPORTED 1
static bool s_jpeg_hw_unavailable;
static struct {void *encoder;uint8_t *input_buf,*jpeg_buf;size_t jpeg_buf_size;} s_jpeg={
    .encoder=(void *)(uintptr_t)1,.input_buf=encoder_input_pixels,.jpeg_buf=encoded,.jpeg_buf_size=sizeof(encoded)};
static int esp_vision_jpeg_ensure_encoder(void) {return ensure_result;}
static int jpeg_del_encoder_engine(void *encoder) {assert(encoder);return ESP_OK;}
static void esp_vision_jpeg_free_cpu_buffer(void) {}
static int esp_vision_jpeg_ensure_buffers(size_t size) {assert(size==1);return 0;}
static int esp_vision_jpeg_encode_cpu(const image_t *image,int quality,uint8_t **data,size_t *size) {
    (void)image;(void)quality;(void)data;(void)size;return ESP_ERR_NO_MEM;
}
static int jpeg_encoder_process(void *encoder,const jpeg_encode_cfg_t *cfg,const uint8_t *input,
                                uint32_t input_size,uint8_t *output,uint32_t capacity,uint32_t *length) {
    assert(encoder && cfg->width==1 && cfg->height==1 && input && input_size==1 && capacity==sizeof(encoded));
    fill_encoded(output);*length=(uint32_t)returned_size;return codec_result;
}
'''
        software = r'''
#define ESP_VISION_HAS_NEW_JPEG_ENCODER 1
#define JPEG_ERR_OK 0
#define JPEG_ROTATE_0D 0
#define DEFAULT_JPEG_ENC_CONFIG() ((jpeg_enc_config_t){0})
#define JPEG_PIXEL_FORMAT_RGB565_LE 2
#define JPEG_PIXEL_FORMAT_GRAY 1
#define JPEG_SUBSAMPLE_420 2
#define JPEG_SUBSAMPLE_GRAY 1
typedef int jpeg_pixel_format_t,esp_new_jpeg_subsampling_t,jpeg_error_t;
typedef void *jpeg_enc_handle_t;
typedef struct {int width,height,src_type,subsampling,rotate;uint8_t quality;bool task_enable;} jpeg_enc_config_t;
static struct {uint8_t *input_buf,*jpeg_buf;size_t jpeg_buf_size;} s_new_jpeg={
    .input_buf=encoder_input_pixels,.jpeg_buf=encoded,.jpeg_buf_size=sizeof(encoded)};
static size_t esp_vision_new_jpeg_capacity(size_t raw_size) {return raw_size+2048;}
static int esp_vision_new_jpeg_ensure_buffers(size_t size) {assert(size==1);return ensure_result;}
static int esp_vision_jpeg_error_to_esp_err(int error) {return error;}
static int jpeg_enc_open(const jpeg_enc_config_t *config,jpeg_enc_handle_t *encoder) {
    assert(config->width==1 && config->height==1 && config->quality==60);
    opened++;*encoder=(void *)(uintptr_t)1;return 0;
}
static int jpeg_enc_process(jpeg_enc_handle_t encoder,const uint8_t *input,int input_size,
                            uint8_t *output,int capacity,int *length) {
    assert(encoder && input && input_size==1 && capacity==sizeof(encoded));
    fill_encoded(output);*length=returned_size;return codec_result;
}
static void jpeg_enc_close(jpeg_enc_handle_t encoder) {assert(encoder);closed++;}
'''
        for backend in ('hardware', 'software'):
            with self.subTest(backend=backend):
                if backend == 'hardware':
                    production = function(source, 'static esp_err_t esp_vision_jpeg_image_format(')
                    production += function(source, 'esp_err_t esp_vision_jpeg_encode(')
                    definitions = hardware
                else:
                    production = function(source, 'static esp_err_t esp_vision_new_jpeg_image_format(')
                    production += function(source, 'static esp_err_t esp_vision_new_jpeg_encode(')
                    production += function(source, 'esp_err_t esp_vision_jpeg_encode(', last=True)
                    definitions = software
                self.compile_run(common + definitions + production + route_stubs + handlers + preview + main)

    def test_unsupported_encoder_also_clears_available_outputs(self):
        source = function((ROOT / 'platform/jpeg.c').read_text(), 'esp_err_t esp_vision_jpeg_encode(', last=True)
        harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef struct {int32_t w,h;uint32_t pixfmt,size;uint8_t *pixels;} image_t;
#define PIXFORMAT_JPEG 5
#include "jpeg_validate.h"
#define ESP_VISION_HAS_NEW_JPEG_ENCODER 0
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_INVALID_RESPONSE 3
'''
        main = r'''
int main(void) {
    uint8_t pixels[1]={80};image_t image={.w=1,.h=1,.pixels=pixels};uint8_t *data=(uint8_t *)(uintptr_t)1;size_t size=123;
    assert(esp_vision_jpeg_encode(&image,60,&data,&size)==ESP_ERR_NOT_SUPPORTED && data==NULL && size==0);
    size=123;
    assert(esp_vision_jpeg_encode(&image,60,NULL,&size)==ESP_ERR_INVALID_ARG && size==0);
    data=(uint8_t *)(uintptr_t)1;
    assert(esp_vision_jpeg_encode(&image,60,&data,NULL)==ESP_ERR_INVALID_ARG && data==NULL);
    return 0;
}
'''
        self.compile_run(harness + source + main)

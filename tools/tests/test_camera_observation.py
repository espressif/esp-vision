# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise production camera publication and passive RPC ownership boundaries."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(signature)


def framebuffer_source():
    return '\n'.join(line for line in (ROOT / 'platform/camera_framebuffer.c').read_text().splitlines()
                     if not line.startswith(('#include', '#ifndef', '#define', '#endif')))


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef void *TaskHandle_t;
typedef struct { int32_t w,h; uint32_t pixfmt,size; uint8_t *_raw,*pixels; } image_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_NOT_SUPPORTED 5
#define MALLOC_CAP_8BIT 2
#define ESP_VISION_MEMORY_PREFER_SPIRAM 1
#define PIXFORMAT_RGB565 2
#define PIXFORMAT_GRAYSCALE 1
#define ESP_VISION_JPEG_QUALITY_LOW 50
static uintptr_t current_task=1;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) {return (TaskHandle_t)current_task;}
static int allocations,frees,fail_all;
static void *esp_vision_memory_malloc(size_t size, unsigned int caps) {
    assert(caps==MALLOC_CAP_8BIT); allocations++;
    return fail_all ? NULL : malloc(size);
}
static void *esp_vision_memory_malloc_preferred(size_t size,unsigned int caps,int preference) {
    assert(preference==ESP_VISION_MEMORY_PREFER_SPIRAM);
    return esp_vision_memory_malloc(size,caps);
}
static void heap_caps_free(void *p) {if(p) frees++; free(p);}
static uint32_t width=4,height=4,pixfmt=PIXFORMAT_GRAYSCALE;
static size_t frame_size=16;
static bool ready=true;
static unsigned capture_calls;
static uint8_t capture_value=0xAA;
static int capture_error;
static bool esp_vision_camera_is_ready(void) {return ready;}
static uint32_t esp_vision_camera_get_width(void) {return width;}
static uint32_t esp_vision_camera_get_height(void) {return height;}
static uint32_t esp_vision_camera_get_pixformat(void) {return pixfmt;}
static size_t esp_vision_camera_frame_size(void) {return frame_size;}
static int esp_vision_camera_capture_into(uint8_t *pixels,size_t size) {
    if(!ready) return ESP_ERR_INVALID_STATE;
    if(!pixels || size<frame_size) return ESP_ERR_INVALID_SIZE;
    capture_calls++; memset(pixels,capture_value,frame_size); return capture_error;
}
'''


class CameraObservationTests(unittest.TestCase):
    def compile_run(self, source):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler unavailable')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'camera.c'
            binary = Path(directory) / 'camera'
            path.write_text(source)
            result = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     str(path), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_passive_rpc_preserves_held_image_and_does_not_acquire(self):
        control = (ROOT / 'platform/ev_control_transport.c').read_text()
        handler = function(control, 'static esp_err_t ev_control_send_debug_capture_frame(')
        # Compile the real handler: an accidental camera init/snapshot call has
        # no harness definition, so it is a hard regression, not a mock success.
        rpc = r'''
static const char *error_code;
static int encode_result,send_result,sends,encodes;
static uint32_t ev_control_json_get_u32(const char *s,const char *k,uint32_t fallback) {
    (void)s;(void)k;return fallback;
}
static const char *esp_err_to_name(int err) {(void)err;return "test";}
static int ev_control_send_error(const char *method,uint32_t seq,const char *code,const char *message) {
    assert(strcmp(method,"debug.capture_frame")==0 && seq==7);(void)message;error_code=code;return -1;
}
static int esp_vision_jpeg_encode(const image_t *img,int quality,uint8_t **jpeg,size_t *size) {
    assert(img->pixels && img->pixels[0]==0xAA && img->pixels[15]==0xAA);
    assert(img->w==4 && img->h==4 && img->pixfmt==PIXFORMAT_GRAYSCALE && img->size==16);
    assert(quality==50);encodes++;*jpeg=img->pixels;*size=16;return encode_result;
}
static int ev_control_send_binary_rsp(const char *method,const char *mime,const void *pixels,
                                      size_t size,uint32_t seq,int w,int h) {
    assert(strcmp(method,"debug.capture_frame")==0 && strcmp(mime,"image/jpeg")==0);
    assert(pixels && size==16 && seq==7 && w==4 && h==4);sends++;return send_result;
}
'''
        main = r'''
int main(void) {
    esp_vision_camera_framebuffer_init0();
    assert(allocations==0);
    assert(ev_control_send_debug_capture_frame(7,"{}")==-1);
    assert(strcmp(error_code,"CAMERA_FRAME_NOT_READY")==0 && capture_calls==0 && encodes==0);
    uint8_t *app=NULL;
    assert(esp_vision_camera_get_snapshot_buffer(16,&app)==ESP_OK);
    assert(esp_vision_camera_capture(app,16)==ESP_OK && capture_calls==1);
    uint8_t *held=app; unsigned calls=capture_calls; int before=allocations;
    for(int i=0;i<4;i++) assert(ev_control_send_debug_capture_frame(7,"{}")==ESP_OK);
    assert(capture_calls==calls && allocations==before && held==app);
    for(size_t i=0;i<16;i++) assert(held[i]==0xAA);
    // Host observes the immutable acquisition, not later application drawing.
    memset(held,0xCC,16);
    assert(ev_control_send_debug_capture_frame(7,"{}")==ESP_OK);
    for(size_t i=0;i<16;i++) assert(held[i]==0xCC);
    // Encoder/send cancellation failures cannot change ownership or reacquire.
    encode_result=9;
    assert(ev_control_send_debug_capture_frame(7,"{}")==-1 && strcmp(error_code,"JPEG_FAILED")==0);
    encode_result=0;send_result=10;
    assert(ev_control_send_debug_capture_frame(7,"{}")==-1 && strcmp(error_code,"SEND_FAILED")==0);
    assert(capture_calls==calls);
    esp_vision_camera_release_snapshot_buffer();
    assert(ev_control_send_debug_capture_frame(7,"{}")==-1 && strcmp(error_code,"CAMERA_FRAME_NOT_READY")==0);
    // Allocation failure belongs to observation, never to the application's capture.
    uint8_t external[16]; fail_all=1;
    assert(esp_vision_camera_capture(external,sizeof(external))==ESP_OK);
    assert(ev_control_send_debug_capture_frame(7,"{}")==-1 && strcmp(error_code,"NO_MEM")==0);
    esp_vision_camera_release_snapshot_buffer();
    return 0;
}
'''
        self.compile_run(PRELUDE + framebuffer_source() + rpc + handler + main)

    def test_external_buffers_reuse_failure_latch_and_reset(self):
        main = r'''
int main(void) {
    image_t observed;
    uint8_t external[32];
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE);
    esp_vision_camera_framebuffer_init0();
    assert(allocations==0 && frees==0);
    assert(esp_vision_camera_get_observed_frame(NULL)==ESP_ERR_INVALID_ARG);
    assert(esp_vision_camera_capture(external,16)==ESP_OK && capture_calls==1);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_OK);
    assert(observed.pixels!=external && observed._raw==NULL && allocations==1);
    uint8_t *published=observed.pixels;
    memset(external,0xCC,sizeof(external));
    assert(observed.pixels[0]==0xAA && observed.pixels[15]==0xAA);
    // Same geometry has one allocation over arbitrarily many application frames.
    for(int i=0;i<10;i++) assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(allocations==1 && frees==0);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_OK && observed.pixels==published);
    capture_error=7;
    assert(esp_vision_camera_capture(external,16)==7);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE);
    capture_error=0;
    esp_vision_camera_invalidate_observed_frame();
    assert(frees==1);
    fail_all=1;
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_NO_MEM);
    int attempts=allocations;
    fail_all=0;
    for(int i=0;i<10;i++) assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(allocations==attempts && esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_NO_MEM);
    esp_vision_camera_invalidate_observed_frame();
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_OK);
    width=8;height=2;pixfmt=PIXFORMAT_RGB565;frame_size=32;
    esp_vision_camera_invalidate_observed_frame();
    assert(esp_vision_camera_capture(external,sizeof(external))==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_OK);
    assert(observed.w==8 && observed.h==2 && observed.pixfmt==PIXFORMAT_RGB565 && observed.size==32);
    esp_vision_camera_framebuffer_init0(); // VM soft reset drops publication/storage.
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE);
    esp_vision_camera_release_snapshot_buffer(); // Idempotent cleanup.
    assert(frees==allocations-1); // one intentional failed allocation
    return 0;
}
'''
        self.compile_run(PRELUDE + framebuffer_source() + main)

    def test_worker_tasks_cannot_capture_free_or_rebind_camera(self):
        main = r'''
int main(void) {
    uint8_t external[16],*snapshot=NULL; image_t observed;
    esp_vision_camera_framebuffer_init0();
    assert(esp_vision_camera_get_snapshot_buffer(16,&snapshot)==ESP_OK);
    assert(esp_vision_camera_capture(external,sizeof(external))==ESP_OK);
    unsigned calls=capture_calls; int before=frees;
    current_task=2;
    assert(!esp_vision_camera_is_vm_task());
    esp_vision_camera_framebuffer_init0();
    esp_vision_camera_release_snapshot_buffer();
    esp_vision_camera_invalidate_observed_frame();
    assert(frees==before);
    assert(esp_vision_camera_get_snapshot_buffer(16,&snapshot)==ESP_ERR_INVALID_STATE && snapshot==NULL);
    assert(esp_vision_camera_capture(external,sizeof(external))==ESP_ERR_INVALID_STATE && capture_calls==calls);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE);
    current_task=1;
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_OK && observed.pixels[0]==0xAA);
    ready=false;
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE);
    esp_vision_camera_release_snapshot_buffer();
    assert(frees==allocations);
    return 0;
}
'''
        self.compile_run(PRELUDE + framebuffer_source() + main)

    def test_malformed_backend_metadata_cannot_publish_out_of_bounds(self):
        main = r'''
int main(void) {
    uint8_t external[16]; image_t observed;
    esp_vision_camera_framebuffer_init0();
    width=5; // frame_size no longer covers the metadata dimensions.
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_SIZE && allocations==0);
    width=UINT32_MAX;
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_SIZE && allocations==0);
    width=4;pixfmt=99;
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_SIZE && allocations==0);
    pixfmt=PIXFORMAT_GRAYSCALE;
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_OK && allocations==1);
    esp_vision_camera_release_snapshot_buffer();
    return 0;
}
'''
        self.compile_run(PRELUDE + framebuffer_source() + main)

    def test_s31_production_reconfiguration_and_deinit_invalidate_publication(self):
        backend = (ROOT / 'boards/ESP32_S31_KORVO/camera.c').read_text()
        signatures = ['void esp_vision_camera_deinit(void)', 'void esp_vision_camera_init0(void)',
                      'esp_err_t esp_vision_camera_set_pixformat(uint32_t pixfmt)',
                      'esp_err_t esp_vision_camera_set_framesize(esp_vision_camera_framesize_t framesize)',
                      'esp_err_t esp_vision_camera_set_hmirror(bool enable)',
                      'esp_err_t esp_vision_camera_set_vflip(bool enable)']
        production = '\n'.join(function(backend, signature) for signature in signatures)
        prelude = PRELUDE.replace('static uint32_t width=4,height=4,pixfmt=PIXFORMAT_GRAYSCALE;', r'''
typedef uint32_t pixformat_t;
typedef enum { SIZE_FIRST, SIZE_SECOND } esp_vision_camera_framesize_t;
static struct { bool initialized,vflip,hmirror; int fd; uint32_t width,height,output_pixfmt; }
    s_camera={.initialized=true,.width=4,.height=4,.output_pixfmt=PIXFORMAT_GRAYSCALE};
''').replace('return width;', 'return s_camera.width;').replace('return height;', 'return s_camera.height;')
        prelude = prelude.replace('return pixfmt;', 'return s_camera.output_pixfmt;')
        # The production setter updates the actual backend state; failure can
        # occur after that mutation and must not leave a stale published frame.
        helpers = r'''
static int config_error;
static void esp_vision_camera_set_defaults(void) {
    s_camera.width=4;s_camera.height=4;s_camera.output_pixfmt=PIXFORMAT_GRAYSCALE;
}
static void esp_vision_camera_cleanup(void) {ready=false;s_camera.initialized=false;}
static int esp_vision_camera_reinit_ppa(void) {return config_error;}
static bool esp_vision_camera_get_hmirror(void) {return s_camera.hmirror;}
static bool esp_vision_camera_get_vflip(void) {return s_camera.vflip;}
static int esp_vision_camera_get_framesize_dimensions(esp_vision_camera_framesize_t size,
                                                      uint32_t *w,uint32_t *h) {
    if(size==SIZE_FIRST){*w=4;*h=4;return ESP_OK;}
    if(size==SIZE_SECOND){*w=8;*h=2;return ESP_OK;}
    return ESP_ERR_NOT_SUPPORTED;
}
'''
        main = r'''
int main(void) {
    uint8_t external[16];image_t observed;
    esp_vision_camera_framebuffer_init0();
    assert(esp_vision_camera_capture(external,16)==ESP_OK && allocations==1);
    assert(esp_vision_camera_set_framesize(SIZE_FIRST)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_OK && frees==0);
    config_error=8;
    assert(esp_vision_camera_set_framesize(SIZE_SECOND)==8);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE && frees==1);
    config_error=0;
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_set_hmirror(true)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE);
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_set_vflip(true)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE);
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_set_pixformat(PIXFORMAT_RGB565)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE);
    assert(esp_vision_camera_set_pixformat(PIXFORMAT_GRAYSCALE)==ESP_OK);
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    int before=frees;
    current_task=2;
    assert(esp_vision_camera_set_framesize(SIZE_FIRST)==ESP_ERR_INVALID_STATE);
    assert(esp_vision_camera_set_pixformat(PIXFORMAT_RGB565)==ESP_ERR_INVALID_STATE);
    assert(esp_vision_camera_set_hmirror(false)==ESP_ERR_INVALID_STATE);
    assert(esp_vision_camera_set_vflip(false)==ESP_ERR_INVALID_STATE);
    esp_vision_camera_deinit();esp_vision_camera_init0();
    assert(frees==before && s_camera.initialized);
    current_task=1;
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_OK);
    esp_vision_camera_deinit();
    assert(frees==before+1 && esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE);
    ready=true;s_camera.initialized=true;
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    esp_vision_camera_init0();
    assert(esp_vision_camera_get_observed_frame(&observed)==ESP_ERR_INVALID_STATE && frees==allocations);
    return 0;
}
'''
        self.compile_run(prelude + framebuffer_source() + helpers + production + main)

    def test_p4_ipa_and_manual_isp_changes_invalidate_before_hardware(self):
        helpers = r'''
typedef int esp_vision_isp_block_t;
typedef struct { int value; } esp_vision_isp_config_t;
typedef struct {
    void (*release_buffers)(void);
    int (*open_device)(void),(*set_input_format)(void);
    void (*update_active_window)(void);
    int (*init_buffers)(void),(*start_stream)(void);
    void (*cleanup)(void);
    void (*close_failed)(void);
} esp_vision_camera_restart_ops_t;
static struct {int fd;bool streaming,initialized;const char *isp_device_path,*device_path;void *ipa_config;}
    s_camera={.fd=3,.streaming=true,.initialized=true,.isp_device_path="isp",.device_path="camera"};
static bool ipa_enabled;
static int ipa_query_result,ipa_change_result,isp_change_result,ipa_queries,ipa_changes,isp_changes;
static int esp_vision_camera_get_ipa(bool *enabled) {ipa_queries++;*enabled=ipa_enabled;return ipa_query_result;}
static bool esp_vision_video_isp_controller_active(void) {return ipa_enabled;}
static void esp_vision_camera_release_buffers(void) {}
static int esp_vision_camera_open_device(void) {return 0;}
static int esp_vision_camera_set_input_format(void) {return 0;}
static void esp_vision_camera_update_active_window(void) {}
static int esp_vision_camera_init_buffers(void) {return 0;}
static int esp_vision_camera_start_stream(void) {return 0;}
static void esp_vision_camera_cleanup(void) {ready=false;}
void esp_vision_camera_deinit(void) {esp_vision_camera_cleanup();}
void esp_vision_camera_close_failed(void) {ready=false;}
static int esp_vision_video_isp_set_controller(bool enable,int *fd,bool *streaming,bool *initialized,
                                               const char *isp,const char *camera,void *config,
                                               const esp_vision_camera_restart_ops_t *ops) {
    (void)config;assert(fd && streaming && initialized && isp && camera && ops);
    image_t frame;assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_INVALID_STATE);
    ipa_changes++;ipa_enabled=enable;
    if(ipa_change_result)ops->cleanup();
    return ipa_change_result;
}
static int esp_vision_video_isp_set(int fd,const char *path,esp_vision_isp_block_t block,
                                    const esp_vision_isp_config_t *config) {
    assert(fd==3 && path && block==0 && config);
    image_t frame;assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_INVALID_STATE);
    isp_changes++;return isp_change_result;
}
'''
        main = r'''
int main(void) {
    uint8_t external[16];image_t frame;esp_vision_isp_config_t config={0};
    esp_vision_camera_framebuffer_init0();
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    // Identical IPA mode is a no-op, retaining the valid publication.
    assert(esp_vision_camera_set_ipa(false)==ESP_OK && ipa_changes==0 && frees==0);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_OK);
    int queries=ipa_queries;
    current_task=2;
    assert(esp_vision_camera_set_ipa(true)==ESP_ERR_INVALID_STATE);
    assert(esp_vision_camera_set_isp(0,&config)==ESP_ERR_INVALID_STATE);
    assert(ipa_queries==queries && ipa_changes==0 && isp_changes==0 && frees==0);
    current_task=1;
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_OK);
    assert(esp_vision_camera_set_ipa(true)==ESP_OK && ipa_changes==1 && frees==1);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_INVALID_STATE);
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    ipa_query_result=8;
    assert(esp_vision_camera_set_ipa(false)==8 && ipa_changes==1);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_OK);
    ipa_query_result=0;ipa_change_result=9;
    assert(esp_vision_camera_set_ipa(false)==9 && ipa_changes==2 && !ready);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_INVALID_STATE);
    // Restart failure cannot expose its old frame after re-initialization.
    ready=true;ipa_change_result=0;
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_INVALID_STATE);
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    esp_vision_camera_invalidate_observed_frame();fail_all=1;
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_NO_MEM);
    assert(esp_vision_camera_set_ipa(false)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_NO_MEM);
    assert(esp_vision_camera_set_ipa(true)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_INVALID_STATE);
    fail_all=0;
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    // Active IPA rejects manual writes without touching observation or hardware.
    assert(esp_vision_camera_set_isp(0,&config)==ESP_ERR_INVALID_STATE && isp_changes==0);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_OK);
    assert(esp_vision_camera_set_ipa(false)==ESP_OK);
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    isp_change_result=10;
    assert(esp_vision_camera_set_isp(0,&config)==10 && isp_changes==1);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_INVALID_STATE);
    // Any actual manual ISP write also releases an allocation failure latch.
    fail_all=1;
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_NO_MEM);
    isp_change_result=0;
    assert(esp_vision_camera_set_isp(0,&config)==ESP_OK && isp_changes==2);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_ERR_INVALID_STATE);
    fail_all=0;
    assert(esp_vision_camera_capture(external,16)==ESP_OK);
    assert(esp_vision_camera_get_observed_frame(&frame)==ESP_OK);
    esp_vision_camera_release_snapshot_buffer();
    assert(frees==allocations-2); // two deliberately failed observation allocations
    return 0;
}
'''
        for board in ('ESP32_P4X_EYE', 'ESP32_P4X_FUNCTION_EV_BOARD'):
            with self.subTest(board=board):
                backend = (ROOT / f'boards/{board}/camera.c').read_text()
                production = '\n'.join(function(backend, signature) for signature in (
                    'esp_err_t esp_vision_camera_set_ipa(bool enable)',
                    'esp_err_t esp_vision_camera_set_isp(esp_vision_isp_block_t block,'))
                self.compile_run(PRELUDE + framebuffer_source() + helpers + production + main)

    def test_python_isp_worker_is_rejected_before_parsing_or_hardware_access(self):
        binding = function((ROOT / 'modules/py_sensor_isp.c').read_text(),
                           'static mp_obj_t sensor_set_isp(')
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef intptr_t mp_obj_t;
typedef struct {size_t used;} mp_map_t;
typedef struct {mp_obj_t value;} mp_map_elem_t;
typedef int esp_vision_isp_block_t;
typedef struct {bool params_changed;} esp_vision_isp_config_t;
typedef struct {int name;} sensor_isp_field_t;
typedef struct {size_t count;const sensor_isp_field_t *fields;} sensor_isp_block_t;
#define ESP_OK 0
#define ESP_VISION_ISP_EXPOSURE 0
#define ESP_VISION_ISP_PIXEL_GAIN 1
#define MP_ERROR_TEXT(x) x
#define MP_OBJ_NEW_QSTR(x) (x)
#define MP_QSTR_enable 0
#define MP_MAP_LOOKUP 1
static const int mp_type_OSError;
static const mp_obj_t mp_const_none=0;
static const sensor_isp_field_t fields[]={{.name=1}};
static const sensor_isp_block_t s_blocks[]={{.count=1,.fields=fields}};
static mp_map_elem_t value={.value=23};
static jmp_buf raised;
static bool main_task;
static int parses,reads,writes;
static bool esp_vision_camera_is_vm_task(void) {return main_task;}
static _Noreturn void mp_raise_msg(const int *type,const char *text) {
    assert(type==&mp_type_OSError && text);longjmp(raised,1);
}
static _Noreturn void mp_raise_ValueError(const char *text) {(void)text;longjmp(raised,2);}
static _Noreturn void mp_raise_TypeError(const char *text) {(void)text;longjmp(raised,3);}
static void mp_arg_check_num(size_t n,size_t used,size_t min,size_t max,bool kw) {
    assert(n==1 && used==1 && min==1 && max==1 && kw);parses++;
}
static esp_vision_isp_block_t sensor_isp_block(mp_obj_t name) {assert(name==1);return 0;}
static int esp_vision_camera_get_ipa(bool *enabled) {*enabled=false;reads++;return 0;}
static int sensor_isp_read(esp_vision_isp_block_t block,esp_vision_isp_config_t *config) {
    (void)block;(void)config;reads++;return 0;
}
static _Noreturn void sensor_isp_raise(int ret) {(void)ret;longjmp(raised,4);}
static mp_map_elem_t *mp_map_lookup(mp_map_t *map,mp_obj_t key,int operation) {
    assert(map && key==1 && operation==MP_MAP_LOOKUP);return &value;
}
static void sensor_isp_field_set(const sensor_isp_field_t *field,esp_vision_isp_config_t *config,mp_obj_t v) {
    assert(field && config && v==23);
}
static int esp_vision_camera_set_isp(esp_vision_isp_block_t block,const esp_vision_isp_config_t *config) {
    assert(block==0 && config->params_changed);writes++;return 0;
}
'''
        main = r'''
int main(void) {
    mp_obj_t args[]={1};mp_map_t kwargs={.used=1};
    if(setjmp(raised)==0) {
        sensor_set_isp(1,args,&kwargs);
        assert(false);
    }
    assert(parses==0 && reads==0 && writes==0);
    main_task=true;
    assert(sensor_set_isp(1,args,&kwargs)==mp_const_none);
    assert(parses==1 && reads==1 && writes==1);
    return 0;
}
'''
        self.compile_run(harness + binding + main)

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile the real P4 shutdown path and the all-board weak fallback."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.tests.test_ov2710_lifecycle import function

ROOT = Path(__file__).resolve().parents[2]


class CameraShutdownTests(unittest.TestCase):
    def compile_run(self, *sources):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler unavailable')
        with tempfile.TemporaryDirectory() as directory:
            files = []
            for i, source in enumerate(sources):
                path = Path(directory) / f'test{i}.c'
                path.write_text(source)
                files.append(str(path))
            binary = Path(directory) / 'test'
            result = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     *files, '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_weak_fallback_and_board_override_link(self):
        declarations = '''
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 2
bool esp_vision_camera_is_vm_task(void);
void esp_vision_camera_deinit(void);
int esp_vision_camera_shutdown(void);
'''
        fallback = declarations + (ROOT / 'platform/camera_lifecycle.c').read_text().replace('#include "camera.h"', '')
        main = declarations + '''
#include <assert.h>
static bool main_vm=true;static int calls;
bool esp_vision_camera_is_vm_task(void){return main_vm;}
void esp_vision_camera_deinit(void){calls++;}
int main(void){assert(esp_vision_camera_shutdown()==0 && calls==1);main_vm=false;
assert(esp_vision_camera_shutdown()==2 && calls==1);return 0;}
'''
        self.compile_run(fallback, main)
        override = 'int esp_vision_camera_shutdown(void){return 19;}\n'
        main = main[:main.index('int main(')] + 'int main(void){assert(esp_vision_camera_shutdown()==19);return 0;}\n'
        self.compile_run(fallback, override, main)
        self.assertIn('${ESP_VISION_ROOT}/platform/camera_lifecycle.c', (ROOT / 'micropython.cmake').read_text())

    def test_real_cleanup_retains_or_latches_failed_ownership(self):
        source = (ROOT / 'boards/ESP32_P4X_EYE/camera.c').read_text()
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define CONFIG_CAMERA_OV2710_STATS_UPDATE_EN 1
#define ESP_VISION_ENABLE_CAMERA_IPA_CONTROL 0
#define ESP_BOARD_DEVICE_NAME_CAMERA "camera"
#define CSI_NAME "MIPI-CSI"
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define VIDIOC_STREAMOFF 2
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(call,...) do {int err=(call);if(err)return err;}while(0)
enum v4l2_buf_type { dummy=0 };
struct {bool initialized,video_initialized,streaming,teardown_failed;int fd,width,height,output_pixfmt;const char *device_path;} s_camera;
typedef struct {const char *name;} sensor_t;
typedef struct {sensor_t *sensor;} esp_video_cam_t;
static sensor_t sensor={"OV2710"};
static int preflight_error,stream_error,close_error,bm_error;
static int gets,preflights,streams,buffers,ppa,closes,bms;
static int esp_video_device_common_get_video_cam(const char *name,esp_video_cam_t *cam){(void)name;gets++;cam->sensor=&sensor;return 0;}
static int esp_vision_ov2710_prepare_delete(sensor_t *value){assert(value==&sensor);preflights++;return preflight_error;}
static int ioctl(int fd,int request,void *arg){assert(fd==5);(void)request;(void)arg;streams++;return stream_error;}
static void esp_vision_camera_release_buffers(void){buffers++;}
static void esp_vision_camera_release_ppa(void){ppa++;}
static int close(int fd){assert(fd==5);closes++;return close_error;}
static int esp_board_manager_deinit_device_by_name(const char *name){(void)name;bms++;return bm_error;}
static bool esp_vision_camera_is_vm_task(void){return true;}
static void esp_vision_camera_invalidate_observed_frame(void){}
static int esp_vision_camera_shutdown(void);
'''
        functions = '\n'.join(function(source, signature) for signature in (
            'static esp_err_t esp_vision_camera_video_deinit(',
            'static esp_err_t esp_vision_camera_cleanup(',
            'esp_err_t esp_vision_camera_shutdown('))
        # Match the real function's linkage in the extracted translation unit.
        prelude = prelude.replace('static int esp_vision_camera_shutdown(void);', 'int esp_vision_camera_shutdown(void);')
        main = r'''
static void begin(void){memset(&s_camera,0,sizeof(s_camera));s_camera.initialized=true;
s_camera.video_initialized=true;s_camera.streaming=true;s_camera.fd=5;s_camera.device_path="/dev/video0";
preflight_error=stream_error=close_error=bm_error=0;gets=preflights=streams=buffers=ppa=closes=bms=0;}
int main(void){
begin();preflight_error=3;assert(esp_vision_camera_shutdown()==3);
assert(!s_camera.initialized && !s_camera.teardown_failed && !streams && !buffers && !ppa && !closes && !bms);
preflight_error=0;assert(esp_vision_camera_shutdown()==0 && bms==1 && s_camera.fd==-1 && !s_camera.video_initialized);
assert(esp_vision_camera_shutdown()==0 && bms==1);
// A start error may already have started the timer without setting streaming.
begin();s_camera.streaming=false;preflight_error=3;
assert(esp_vision_camera_shutdown()==3 && preflights==1 && !bms && !closes);
begin();stream_error=1;assert(esp_vision_camera_shutdown()==ESP_FAIL);
assert(!s_camera.initialized && !s_camera.teardown_failed && !buffers && !closes && !bms);
stream_error=0;assert(esp_vision_camera_shutdown()==0 && bms==1);
begin();close_error=1;assert(esp_vision_camera_shutdown()==ESP_FAIL);
assert(s_camera.fd==-1 && s_camera.teardown_failed && !bms);
int prior_gets=gets;assert(esp_vision_camera_shutdown()==ESP_ERR_INVALID_STATE && gets==prior_gets && closes==1);
begin();bm_error=1;assert(esp_vision_camera_shutdown()==1 && s_camera.teardown_failed);
prior_gets=gets;assert(esp_vision_camera_shutdown()==ESP_ERR_INVALID_STATE && gets==prior_gets && bms==1);
return 0;}
'''
        self.compile_run(prelude + functions + main)


if __name__ == '__main__':
    unittest.main()

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise real lazy snapshot storage and LCD borrowed-buffer completion."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class FramebufferLifetimeTests(unittest.TestCase):
    def compile_run(self, source):
        compiler = shutil.which('cc')
        if compiler is None:
            self.skipTest('native C compiler unavailable')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'framebuffer.c'
            binary = Path(directory) / 'framebuffer'
            path.write_text(source)
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True)

    def test_snapshot_lazy_reuse_failure_and_release(self):
        source = (ROOT / 'platform/camera_framebuffer.c').read_text()
        source = '\n'.join(line for line in source.splitlines() if not line.startswith(('#include', '#ifndef', '#define', '#endif')))
        self.compile_run(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_SIZE 4
#define PIXFORMAT_GRAYSCALE 1
#define PIXFORMAT_RGB565 2
#define ESP_VISION_MEMORY_PREFER_SPIRAM 1
typedef void *TaskHandle_t;
typedef struct { int32_t w,h; uint32_t pixfmt,size; uint8_t *_raw,*pixels; } image_t;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) {return (TaskHandle_t)1;}
static bool esp_vision_camera_is_ready(void) {return true;}
static uint32_t esp_vision_camera_get_width(void) {return 4;}
static uint32_t esp_vision_camera_get_height(void) {return 4;}
static uint32_t esp_vision_camera_get_pixformat(void) {return PIXFORMAT_GRAYSCALE;}
static size_t esp_vision_camera_frame_size(void) {return 16;}
static int esp_vision_camera_capture_into(uint8_t *p,size_t n) {(void)p;(void)n;return 0;}
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
static int allocations, frees, fail_all, fail_psram;
static void *heap_caps_malloc(size_t n,int caps) {
    allocations++;
    if(fail_all || (fail_psram && (caps&MALLOC_CAP_SPIRAM))) return NULL;
    return malloc(n);
}
static void heap_caps_free(void *p) { if(p) frees++; free(p); }
static void *esp_vision_memory_malloc(size_t n, unsigned int required_caps) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | required_caps);
    if (p == NULL) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | required_caps);
    return p;
}
static void *esp_vision_memory_malloc_preferred(size_t n,unsigned int caps,int preference) {
    assert(preference==ESP_VISION_MEMORY_PREFER_SPIRAM);return esp_vision_memory_malloc(n,caps);
}
''' + source + r'''
int main(void) {
    esp_vision_camera_framebuffer_init0();
    uint8_t *first=NULL,*next=NULL;
    assert(allocations==0 && frees==0);
    assert(esp_vision_camera_get_snapshot_buffer(0,&first)==ESP_ERR_INVALID_ARG);
    assert(esp_vision_camera_get_snapshot_buffer(100,NULL)==ESP_ERR_INVALID_ARG);
    assert(allocations==0);
    assert(esp_vision_camera_get_snapshot_buffer(100,&first)==0 && first);
    assert(esp_vision_camera_get_snapshot_buffer(80,&next)==0 && next==first);
    assert(allocations==1 && frees==0);
    first[0]=42; fail_all=1;
    assert(esp_vision_camera_get_snapshot_buffer(200,&next)==ESP_ERR_NO_MEM && next==NULL);
    assert(frees==0 && first[0]==42);
    assert(esp_vision_camera_get_snapshot_buffer(100,&next)==0 && next==first);
    fail_all=0;
    assert(esp_vision_camera_get_snapshot_buffer(200,&next)==0 && next!=first);
    assert(frees==1);
    esp_vision_camera_release_snapshot_buffer();
    esp_vision_camera_release_snapshot_buffer();
    assert(frees==2);
    fail_psram=1;
    int before=allocations;
    assert(esp_vision_camera_get_snapshot_buffer(64,&next)==0 && next);
    assert(allocations==before+2);
    esp_vision_camera_release_snapshot_buffer();
    assert(frees==3);
    return 0;
}
''')

    def test_lcd_submission_drains_borrowed_dma_even_after_draw_error(self):
        source = (ROOT / 'platform/display.c').read_text()
        source = source[source.index('static esp_err_t esp_vision_display_submit_frame(void)'):
                        source.index('\nvoid esp_vision_display_init0(void)')]
        self.compile_run(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_VISION_USE_BOARD_MANAGER 1
static bool s_board_display_uses_spi=true;
static uint16_t pixels[6]={1,2,3,4,5,6};
static struct { void *panel_handle,*io_handle; int width,height; uint16_t *framebuffer; } s_display;
static int pending,draw_error,drain_error,drains;
static int esp_lcd_panel_draw_bitmap(void *panel,int x,int y,int w,int h,const void *p) {
    assert(panel==s_display.panel_handle && x==0 && y==0 && w==3 && h==2);
    assert(p==pixels && !pending); pending=1; return draw_error;
}
static int esp_lcd_panel_io_tx_param(void *io,int command,const void *p,size_t n) {
    assert(io==s_display.io_handle && command==-1 && p==NULL && n==0);
    assert(pending && pixels[0]==1 && pixels[5]==6);
    pending=0; drains++; return drain_error;
}
''' + source + r'''
int main(void) {
    s_display.panel_handle=pixels; s_display.io_handle=pixels;
    s_display.width=3;s_display.height=2;s_display.framebuffer=pixels;
    assert(esp_vision_display_submit_frame()==0 && !pending && drains==1);
    draw_error=7;
    assert(esp_vision_display_submit_frame()==7 && !pending && drains==2);
    draw_error=0;drain_error=8;
    assert(esp_vision_display_submit_frame()==8 && !pending && drains==3);
    s_display.io_handle=NULL;
    assert(esp_vision_display_submit_frame()==0 && drains==3);
    pending=0;s_display.io_handle=pixels;s_board_display_uses_spi=false;
    assert(esp_vision_display_submit_frame()==0 && drains==3);
    return 0;
}
''')

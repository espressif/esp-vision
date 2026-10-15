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
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        self.compile_run(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static int allocations, frees, fail_all, fail_psram;
static void *heap_caps_malloc(size_t n,int caps) {
    allocations++;
    if(fail_all || (fail_psram && (caps&MALLOC_CAP_SPIRAM))) return NULL;
    return malloc(n);
}
static void heap_caps_free(void *p) { if(p) frees++; free(p); }
''' + source + r'''
int main(void) {
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

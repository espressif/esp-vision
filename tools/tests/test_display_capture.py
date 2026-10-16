#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile production LCD-frame and RPC handlers; no second protocol client."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class DisplayCaptureTests(unittest.TestCase):
    def test_last_display_frame_errors_and_binary_response(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler unavailable')
        display = (ROOT / 'platform/display.c').read_text()
        view = display[display.index('esp_err_t esp_vision_display_get_frame('):
                       display.index('\nuint32_t esp_vision_display_get_width(')]
        control = (ROOT / 'platform/ev_control_transport.c').read_text()
        handler = control[control.index('static esp_err_t ev_control_send_debug_capture_screen('):
                          control.index('\nstatic esp_err_t ev_control_send_storage_writer_control(')]
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_INVALID_STATE 3
#define PIXFORMAT_RGB565 4
#define ESP_VISION_JPEG_QUALITY_LOW 60
#define ESP_VISION_LCD_WIDTH PANEL_WIDTH
#define ESP_VISION_LCD_HEIGHT 240
typedef struct { int w, h, pixfmt; size_t size; uint8_t *data; } image_t;
static struct { bool ready; uint16_t *framebuffer; int width, height; size_t framebuffer_size; } s_display;
static uint16_t pixels[6] = {0xf800,0x07e0,0x001f,0xffff,0,0x1234};
static uint8_t jpeg[] = {0xff,0xd8,0xff,0xd9};
static const char *error_code;
static int encode_result, sends, quality;
static uint32_t ev_control_json_get_u32(const char *request, const char *key, uint32_t fallback) {
    (void)key; return strcmp(request,"invalid") == 0 ? 101 : fallback;
}
static const char *esp_err_to_name(int err) { (void)err; return "test"; }
static int ev_control_send_error(const char *method, uint32_t seq, const char *code, const char *message) {
    assert(strcmp(method,"debug.capture_screen") == 0 && seq == 7);
    (void)message; error_code=code; return -1;
}
static int esp_vision_jpeg_encode(const image_t *frame, int q, uint8_t **out, size_t *length) {
    assert(frame->w==3 && frame->h==2 && frame->size==sizeof(pixels));
    assert(frame->pixfmt==PIXFORMAT_RGB565 && frame->data==(uint8_t *)pixels);
    assert(memcmp(frame->data,pixels,sizeof(pixels))==0);
    quality=q; *out=jpeg; *length=sizeof(jpeg); return encode_result;
}
static int ev_control_send_binary_rsp(const char *method, const char *mime, const void *data,
                                      size_t size, uint32_t seq, int32_t width, int32_t height) {
    assert(strcmp(method,"debug.capture_screen")==0 && strcmp(mime,"image/jpeg")==0);
    assert(data==jpeg && size==sizeof(jpeg) && seq==7 && width==3 && height==2);
    sends++; return ESP_OK;
}
'''
        main = r'''
int main(void) {
    image_t frame;
    assert(esp_vision_display_get_frame(NULL)==ESP_ERR_INVALID_ARG);
    if (PANEL_WIDTH==0) {
        assert(esp_vision_display_get_frame(&frame)==ESP_ERR_NOT_SUPPORTED);
        ev_control_send_debug_capture_screen(7, "{}");
        assert(strcmp(error_code,"NOT_SUPPORTED")==0 && sends==0);
        return 0;
    }
    assert(esp_vision_display_get_frame(&frame)==ESP_ERR_INVALID_STATE);
    ev_control_send_debug_capture_screen(7,"{}");
    assert(strcmp(error_code,"DISPLAY_NOT_READY")==0 && sends==0);
    s_display.ready=true; s_display.width=3; s_display.height=2;
    s_display.framebuffer=pixels; s_display.framebuffer_size=sizeof(pixels);
    assert(ev_control_send_debug_capture_screen(7,"{}")==ESP_OK);
    assert(sends==1 && quality==60);
    ev_control_send_debug_capture_screen(7,"invalid");
    assert(strcmp(error_code,"INVALID_ARGUMENT")==0 && sends==1);
    encode_result=8;
    ev_control_send_debug_capture_screen(7,"{}");
    assert(strcmp(error_code,"JPEG_FAILED")==0 && sends==1);
    s_display.framebuffer=NULL; s_display.ready=false;
    ev_control_send_debug_capture_screen(7,"{}");
    assert(strcmp(error_code,"DISPLAY_NOT_READY")==0 && sends==1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'lcd.c'
            source.write_text(prelude + view + handler + main)
            for panel_width in (0, 320):
                binary = Path(directory) / f'lcd-{panel_width}'
                subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                f'-DPANEL_WIDTH={panel_width}', str(source), '-o', str(binary)],
                               check=True, capture_output=True, text=True)
                subprocess.run([str(binary)], check=True, capture_output=True)

    def test_failed_initial_clear_releases_resources_and_allows_retry(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler unavailable')
        production = (ROOT / 'platform/display.c').read_text()
        init = production[production.index('esp_err_t esp_vision_display_init(uint32_t'):
                          production.index('\nvoid esp_vision_display_deinit(void)')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED 1
#define ESP_VISION_LCD_WIDTH 240
#define ESP_VISION_LCD_HEIGHT 240
static struct { bool ready; uint32_t width,height,backlight; int io_handle,panel_handle; } s_display;
static int allocations, releases, clear_result;
static int esp_vision_display_alloc_framebuffer(uint32_t w,uint32_t h) { assert(w==240 && h==240); allocations++; return 0; }
static void esp_vision_display_release_framebuffer(void) { releases++; }
static int esp_vision_board_display_backlight_init(void) { return 0; }
static int esp_vision_board_display_init_panel(uint32_t w,uint32_t h,int *io,int *panel) { (void)w;(void)h;*io=1;*panel=2;return 0; }
static uint32_t esp_vision_display_clamp_backlight(uint32_t level) { return level>100?100:level; }
static void esp_vision_board_display_set_backlight(uint32_t level) { (void)level; }
static int esp_vision_display_set_backlight(uint32_t level) { s_display.backlight=level; return 0; }
static int esp_vision_display_clear(bool off) { assert(!off); return clear_result; }
static void esp_vision_display_deinit(void) { s_display.ready=false;s_display.io_handle=0;s_display.panel_handle=0;esp_vision_display_release_framebuffer(); }
'''
        main = r'''
int main(void) {
    clear_result=7;
    assert(esp_vision_display_init(0,0,100)==7);
    assert(!s_display.ready && s_display.io_handle==0 && s_display.panel_handle==0);
    assert(allocations==1 && releases==1);
    clear_result=0;
    assert(esp_vision_display_init(0,0,100)==0);
    assert(s_display.ready && allocations==2 && releases==1);
    assert(esp_vision_display_init(0,0,40)==0);
    assert(allocations==2 && s_display.backlight==40);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'display-init.c'
            binary=Path(directory)/'display-init'
            source.write_text(harness+init+main)
            subprocess.run([compiler,'-std=c11','-Wall','-Wextra','-Werror',str(source),'-o',str(binary)],
                           check=True,capture_output=True,text=True)
            subprocess.run([str(binary)],check=True,capture_output=True)

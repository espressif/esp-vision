# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise actual Mosaico RGB565 submission and capture failure boundaries."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MosaicoDisplayBytesTests(unittest.TestCase):
    def test_dma_keeps_transport_order_until_drain_and_restores_capture_order(self):
        board = (ROOT / 'boards/ESP32_S31_MOSAICO/bmgr/setup_device.c').read_text()
        callback = board[board.index('static esp_err_t esp_vision_mosaico_draw_bitmap('):
                         board.index('\nstatic const co5300_lcd_init_cmd_t')]
        display = (ROOT / 'platform/display.c').read_text()
        submit = display[display.index('static esp_err_t esp_vision_display_submit_frame('):
                         display.index('\nvoid esp_vision_display_init0(')]
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int esp_err_t;
typedef int esp_lcd_panel_t;
#define ESP_OK 0
static int panel, io, draw_result, drain_result, drains;
static int *s_co5300_io=&io;
static uint16_t pixels[4];
static const uint16_t native[4]={0x18c3,0xf800,0x07e0,0x001f};
static const uint16_t wire[4]={0xc318,0x00f8,0xe007,0x1f00};
static struct {
    bool frame_valid;
    int width,height;
    int *io_handle;
    esp_lcd_panel_t *panel_handle;
    uint16_t *framebuffer;
} s_display;
static int fake_draw(esp_lcd_panel_t *p,int x,int y,int end_x,int end_y,const void *data) {
    assert(p==&panel && x==0 && y==0 && end_x==2 && end_y==2);
    assert(data==pixels && memcmp(pixels,wire,sizeof(pixels))==0);
    assert(!s_display.frame_valid && !drains);
    return draw_result;
}
static int (*s_co5300_draw_bitmap)(esp_lcd_panel_t *,int,int,int,int,const void *)=fake_draw;
static int esp_lcd_panel_io_tx_param(int *handle,int cmd,const void *data,size_t count) {
    assert(handle==&io && cmd==-1 && data==NULL && !count);
    assert(!s_display.frame_valid);
    // The first drain must see wire order. Native order is safe only after it
    // succeeds; an error must leave borrowed DMA storage untouched.
    assert(memcmp(pixels,(!drains || drain_result) ? wire : native,sizeof(pixels))==0);
    drains++;
    return drain_result;
}
'''
        source += callback
        source += r'''
static int esp_lcd_panel_draw_bitmap(esp_lcd_panel_t *p,int x,int y,int ex,int ey,const void *data) {
    return esp_vision_mosaico_draw_bitmap(p,x,y,ex,ey,data);
}
'''
        source += submit
        source += r'''
static void prepare(int draw_error,int drain_error) {
    memcpy(pixels,native,sizeof(pixels));
    draw_result=draw_error; drain_result=drain_error; drains=0; io=7;
    s_display.width=2;s_display.height=2;s_display.io_handle=&io;
    s_display.panel_handle=&panel;s_display.framebuffer=pixels;
    s_display.frame_valid=true;
}
int main(void) {
    prepare(0,0);
    assert(esp_vision_display_submit_frame()==0);
    assert(s_display.frame_valid && drains==2 && memcmp(pixels,native,sizeof(pixels))==0);
    // Repeated rendering must not alternate the framebuffer's byte order.
    drains=0;
    assert(esp_vision_display_submit_frame()==0);
    assert(s_display.frame_valid && memcmp(pixels,native,sizeof(pixels))==0);
    prepare(11,0);
    assert(esp_vision_display_submit_frame()==11);
    assert(!s_display.frame_valid && drains==2 && memcmp(pixels,native,sizeof(pixels))==0);
    prepare(0,12);
    assert(esp_vision_display_submit_frame()==12);
    assert(!s_display.frame_valid && drains==2 && memcmp(pixels,wire,sizeof(pixels))==0);
    prepare(11,12);
    assert(esp_vision_display_submit_frame()==12);
    assert(!s_display.frame_valid && memcmp(pixels,wire,sizeof(pixels))==0);
    prepare(0,0);
    assert(esp_vision_display_submit_frame()==0 && s_display.frame_valid);
    return 0;
}
'''
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='mosaico-display-') as directory:
            path = Path(directory) / 'display.c'
            binary = Path(directory) / 'display-test'
            path.write_text(source)
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Execute the complete production conversion binding with codec fault injection."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_image_io_lifetime import RUNTIME, cleanup_code
from test_jpeg_validation import function

ROOT = Path(__file__).resolve().parents[2]

SHIMS = r'''
#define MP_ROM_NONE NULL
#define MP_ARG_OBJ 1
#define MP_ARG_KW_ONLY 2
#define MP_ARG_INT 4
#define MP_ARG_BOOL 8
#define JPEG_SUBSAMPLING_AUTO 0
#define PIXFORMAT_INVALID 255
#define PIXFORMAT_GRAYSCALE 0
#define PIXFORMAT_RGB565 1
#define PIXFORMAT_JPEG 9
#define PIXFORMAT_PNG 8
#define IMAGE_HINT_CENTER 1
#define IMAGE_HINT_SCALE_ASPECT_KEEP 2
#define IMAGE_HINT_SCALE_ASPECT_EXPAND 4
#define IMAGE_HINT_SCALE_ASPECT_IGNORE 8
#define IMAGE_HINT_BLACK_BACKGROUND 16
#define IMAGE_HINT_TRANSPOSE 32
#define IMAGE_HINT_AREA 64
#define IMAGE_HINT_BICUBIC 128
#define IMAGE_HINT_BILINEAR 256
#define IMAGE_HINT_EXTRACT_RGB_CHANNEL_FIRST 512
#define IMAGE_HINT_APPLY_COLOR_PALETTE_FIRST 1024
#define PY_ASSERT_TRUE_MSG(c,m) do {if(!(c))nlr_raise(m);}while(0)
enum {MP_QSTR_x_scale,MP_QSTR_y_scale,MP_QSTR_roi,MP_QSTR_rgb_channel,MP_QSTR_alpha,MP_QSTR_color_palette,MP_QSTR_alpha_palette,MP_QSTR_hint,MP_QSTR_copy,MP_QSTR_quality,MP_QSTR_subsampling};
typedef int pixformat_t;
typedef mp_obj_t mp_rom_obj_t;
typedef union {mp_obj_t u_obj,u_rom_obj;intptr_t u_int;bool u_bool;} mp_arg_val_t;
typedef struct {int qstr,flags;mp_arg_val_t value;} mp_arg_t;
typedef struct {int x,y,w,h;} rectangle_t;
typedef struct {
    int w,h;
    union {unsigned pixfmt;struct {unsigned is_color:1;unsigned unused:2;unsigned is_compressed:1;unsigned is_bayer:1;unsigned is_yuv:1;};};
    uint32_t size;
    union {uint8_t *pixels,*data;};
} image_t;
static void *native[64];static int depth,mode,umm_depth,umm_marks;static void *umm_data;static bool make_copy,scale;
static image_t result;
static uint8_t copied[512];
enum {PNG_DISABLED=1,JPEG_FAIL,DRAW_FAIL,ALLOC_FAIL,OVERSIZE,FRAMEBUFFER_FAIL,SUCCESS,UMM_MARK_FAIL,CODEC_RAISE};
static void umm_alloc_mark(void){
    if(mode==UMM_MARK_FAIL)nlr_raise("umm mark failure");
    assert(!umm_depth);umm_depth=1;umm_marks++;
}
static void umm_alloc_free_till_mark(void){assert(umm_depth);free(umm_data);umm_data=NULL;umm_depth=0;}

static void fb_alloc_mark(void){assert(depth<64);native[depth++]=NULL;}
static void *fb_alloc(size_t n,int hint){(void)hint;assert(depth<64);void *p=malloc(n);assert(p);native[depth++]=p;return p;}
static void fb_free(void){assert(depth);free(native[--depth]);}
static void fb_alloc_free_till_mark(void){while(depth){bool mark=!native[depth-1];fb_free();if(mark)return;}}
static image_t *py_image_cobj(mp_obj_t obj){return obj;}
static void mp_arg_parse_all(size_t n,const mp_obj_t *p,mp_map_t *k,size_t count,const mp_arg_t *allowed,mp_arg_val_t *out){
    (void)n;(void)p;(void)k;
    for(size_t i=0;i<count;i++)out[i]=allowed[i].value;
    out[MP_QSTR_copy].u_bool=make_copy;
}
static rectangle_t py_helper_arg_to_roi(mp_obj_t o,image_t *i){(void)o;return (rectangle_t){0,0,i->w,i->h};}
static void py_helper_arg_to_scale(mp_obj_t x,mp_obj_t y,float *sx,float *sy){(void)x;(void)y;*sx=scale?2:1;*sy=1;}
static const void *py_helper_arg_to_palette(mp_obj_t o,int fmt){(void)o;(void)fmt;return NULL;}
static int fast_floorf(float x){return (int)x;}
static int imlib_bayer_shift(int f,int x,int y,bool t){(void)x;(void)y;(void)t;return f;}
static int imlib_yuv_shift(int f,int x){(void)x;return f;}
static uint32_t image_size(image_t *i){return i->is_compressed?i->size:i->w*i->h*2;}
static void imlib_draw_image(image_t *dst,image_t *src,int x,int y,float xs,float ys,rectangle_t *roi,int channel,int alpha,const void *palette,const void *ap,int hint,void *a,void *b,void *c,void *d){
    (void)src;(void)x;(void)y;(void)xs;(void)ys;(void)roi;(void)channel;(void)alpha;(void)palette;(void)ap;(void)hint;(void)a;(void)b;(void)c;(void)d;
    fb_alloc(21,0);if(mode==DRAW_FAIL)nlr_raise("draw failure");memset(dst->data,0x5c,image_size(dst));
}
static bool png_compress(image_t *src,image_t *dst){
    (void)src;if(umm_depth){umm_data=malloc(23);assert(umm_data);}
    if(mode==CODEC_RAISE)nlr_raise("codec failure");
    if(mode==PNG_DISABLED)nlr_raise("PNG encoder is not enabled");
    dst->size=mode==OVERSIZE?300:16;dst->data=fb_alloc(dst->size,0);memset(dst->data,0x69,dst->size);return false;
}
static bool jpeg_compress(image_t *src,image_t *dst,int q,bool f,int sub){
    (void)q;(void)f;(void)sub;png_compress(src,dst);return mode==JPEG_FAIL;
}
static void image_alloc(image_t *i,size_t n){if(mode==ALLOC_FAIL)nlr_raise("image allocation failure");assert(n<=sizeof(copied));i->data=copied;}
static void py_helper_update_framebuffer(image_t *i){(void)i;if(mode==FRAMEBUFFER_FAIL)nlr_raise("framebuffer update failure");}
static mp_obj_t py_image_from_struct(image_t *i){result=*i;return &result;}
'''

CASES = r'''
int main(int argc,char **argv){
    assert(argc==6);int source_format=atoi(argv[5]);mode=atoi(argv[1]);int target=atoi(argv[2]);make_copy=atoi(argv[3]);scale=atoi(argv[4]);
    uint8_t pixels[512];memset(pixels,0x25,sizeof(pixels));
    for(int repeat=0;repeat<32;repeat++){
        image_t source={.w=8,.h=8,.pixfmt=source_format,.size=64,.data=pixels};
        mp_obj_t args[]={&source};
        nlr_buf_t nlr;
        if(!nlr_push(&nlr)){
            mp_obj_t answer=py_image_to(target,MP_ROM_NONE,false,1,args,NULL);
            assert(mode==SUCCESS && answer==&result);nlr_pop();
            if(target==PIXFORMAT_PNG || target==PIXFORMAT_JPEG)assert(result.size==16 && result.data[0]==0x69);
            else assert(result.data[0]==0x5c);
        }else{
            assert(mode!=SUCCESS);
            const char *expected=mode==PNG_DISABLED?"PNG encoder is not enabled":mode==JPEG_FAIL?"Compression Failed!":mode==DRAW_FAIL?"draw failure":mode==ALLOC_FAIL?"image allocation failure":mode==OVERSIZE?"The image doesn't fit in the frame buffer!":mode==UMM_MARK_FAIL?"umm mark failure":mode==CODEC_RAISE?"codec failure":"framebuffer update failure";
            assert(!strcmp(nlr.ret_val,expected));
            // Failure before final publication must not replace source metadata.
            assert(source.pixfmt==(unsigned)source_format && source.data==pixels);
        }
        assert(!callbacks && depth==0 && !umm_depth && !umm_data);
        if(target!=PIXFORMAT_PNG && source_format!=PIXFORMAT_PNG)assert(!umm_marks);
        else if(mode!=UMM_MARK_FAIL)assert(umm_marks==repeat+1);
    }
    puts("PASS conversion");return 0;
}
'''


class ImageConversionLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler unavailable')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        path = Path(cls.directory.name)
        source = (ROOT / 'modules/py_image.c').read_text()
        (path / 'test.c').write_text(RUNTIME + SHIMS + cleanup_code(source)
                                     + function(source, 'static mp_obj_t py_image_to(') + CASES)
        cls.executable = path / 'test'
        flags = os.environ.get('IMAGE_LIFETIME_CFLAGS', '').split()
        result = subprocess.run([compiler, '-std=gnu11', '-O0', '-g', '-Wall', '-Wextra',
                                 '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                                 *flags, str(path / 'test.c'), '-o', str(cls.executable)], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, mode, target=8, copy=False, scale=False, source=1):
        result = subprocess.run([str(self.executable), str(mode), str(target), str(int(copy)), str(int(scale)), str(source)],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS conversion', result.stdout)

    def test_umm_mark_allocation_failure_reclaims_framebuffer_mark(self):
        self.run_case(8)

    def test_png_codec_exception_reclaims_umm_and_framebuffer_scopes(self):
        self.run_case(9)

    def test_png_decode_exception_reclaims_both_scopes(self):
        self.run_case(3, target=0, source=8, copy=True)

    def test_png_disabled_repeatedly_reclaims_native_mark(self):
        self.run_case(1)

    def test_jpeg_encoder_failure_reclaims_temporary_allocations(self):
        self.run_case(2, target=9)

    def test_compressed_resize_draw_failure(self):
        self.run_case(3, scale=True)

    def test_uncompressed_conversion_draw_failure(self):
        self.run_case(3, target=0)

    def test_copy_allocation_failure_after_compression(self):
        self.run_case(4, copy=True)

    def test_in_place_capacity_failure_after_compression(self):
        self.run_case(5)

    def test_framebuffer_publication_failure(self):
        self.run_case(6)

    def test_successful_compressed_and_raw_copy_and_in_place(self):
        for target in (0, 8, 9):
            for copy in (False, True):
                with self.subTest(target=target, copy=copy):
                    self.run_case(7, target=target, copy=copy)


if __name__ == '__main__':
    unittest.main()

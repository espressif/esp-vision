# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Run production file/geometry code with deterministic stream/NLR fault injection.

The stream shim models synchronous MicroPython callbacks, including callbacks
that raise directly instead of returning MP_STREAM_ERROR. It is not a FAT or
JPEG decoder emulator; hardware acceptance covers those implementations.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_jpeg_validation import BASELINE, function

ROOT = Path(__file__).resolve().parents[2]

RUNTIME = r'''
#pragma once
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define NORETURN __attribute__((noreturn))
#define MP_ERROR_TEXT(s) (s)
#define MP_ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MP_OBJ_NULL NULL
#define MP_OBJ_FROM_PTR(p) ((mp_obj_t)(p))
#define MP_OBJ_TO_PTR(p) ((void *)(p))
#define FB_ALLOC_PREFER_SIZE 1
#define FB_ALLOC_NO_HINT 0
#define __REV16(x) __builtin_bswap16(x)
#define OMV_ATTR_ALWAYS_INLINE
#define OMV_MIN(x,y) ((x)<(y)?(x):(y))
typedef void *mp_obj_t;
typedef const char *mp_rom_error_text_t;
typedef uintptr_t mp_uint_t;
typedef int mp_map_t;
static const int mp_const_empty_map=0;
static const int mp_type_OSError=1, mp_type_MemoryError=2, mp_type_ValueError=3;
typedef struct callback {struct callback *prev; void (*fun)(void *);} nlr_jump_callback_node_t;
typedef struct nlr {struct nlr *prev; nlr_jump_callback_node_t *boundary; jmp_buf jump; void *ret_val;} nlr_buf_t;
static nlr_buf_t *nlr_top;
static nlr_jump_callback_node_t *callbacks;
#define nlr_push(n) (((n)->prev=nlr_top),((n)->boundary=callbacks),(nlr_top=(n)),setjmp((n)->jump))
static void nlr_pop(void) {assert(nlr_top);nlr_top=nlr_top->prev;}
static void nlr_push_jump_callback(nlr_jump_callback_node_t *n,void (*fn)(void *)) {n->prev=callbacks;n->fun=fn;callbacks=n;}
static void nlr_pop_jump_callback(bool run) {nlr_jump_callback_node_t *n=callbacks;assert(n);callbacks=n->prev;if(run)n->fun(n);}
NORETURN static void nlr_raise(void *value) {
    nlr_buf_t *target=nlr_top;assert(target);
    while(callbacks!=target->boundary)nlr_pop_jump_callback(true);
    nlr_top=target->prev;target->ret_val=value;longjmp(target->jump,1);
}
NORETURN static void mp_raise_msg(const int *type,const char *msg) {(void)type;nlr_raise((void *)msg);}
#define MP_STREAM_OP_IOCTL 1
#define MP_STREAM_OP_READ 2
#define MP_STREAM_OP_WRITE 4
#define MP_STREAM_SEEK 1
#define MP_STREAM_FLUSH 2
#define MP_STREAM_ERROR ((mp_uint_t)-1)
struct mp_stream_seek_t {off_t offset;int whence;};
typedef struct {mp_uint_t (*read)(mp_obj_t,void *,mp_uint_t,int *);mp_uint_t (*write)(mp_obj_t,const void *,mp_uint_t,int *);mp_uint_t (*ioctl)(mp_obj_t,mp_uint_t,mp_uint_t,int *);} mp_stream_p_t;
static mp_obj_t mp_obj_new_str_from_cstr(const char *s){return (void *)s;}
static mp_obj_t mp_obj_new_str(const char *s,size_t n){(void)n;return (void *)s;}
static const char *mp_obj_str_get_str(mp_obj_t s){return s;}
static const char *qstr_from_str(const char *s){return s;}
static mp_obj_t mp_load_attr(mp_obj_t o,const char *s){(void)s;return o;}
static mp_obj_t mp_call_function_0(mp_obj_t o){return o;}
static const mp_stream_p_t *mp_get_stream_raise(mp_obj_t,int);
static mp_obj_t mp_vfs_open(size_t,mp_obj_t *,mp_map_t *);
static mp_obj_t mp_stream_close(mp_obj_t);
static void fb_alloc_mark(void);
static void fb_alloc_free_till_mark(void);
static void *fb_alloc_all(uint32_t *,int);
static void fb_free(void);
static void fb_free_buffer(void *);
static void umm_alloc_mark(void);
static void umm_alloc_free_till_mark(void);
'''

SHIMS = r'''
static void umm_alloc_mark(void){}
static void umm_alloc_free_till_mark(void){}
static unsigned char input[]={INPUT_BYTES};
static struct stream {size_t pos;bool open;} streams[512];
static int opens,closes,close_calls,reads,seeks,writes;
static int fail_open,fail_read,fail_seek,fail_write,fail_close,fail_alloc;
static bool direct_error,short_read,check_abort_order,close_before_release;
static struct {void *ptr;bool mark;} allocations[128];
static int depth;
static bool failing(int count,int failure,const char *msg) {
    if(count!=failure)return false;
    if(direct_error)nlr_raise((void *)msg);
    return true;
}
static mp_obj_t mp_vfs_open(size_t n,mp_obj_t *a,mp_map_t *m) {
    (void)a;(void)m;assert(n==2);
    if(opens+1==fail_open)nlr_raise("open failure");
    assert(opens<512);struct stream *s=&streams[opens++];*s=(struct stream){.open=true};return s;
}
static mp_obj_t mp_stream_close(mp_obj_t obj) {
    struct stream *s=obj;close_calls++;if(!s->open)return NULL;
    if(check_abort_order){assert(!file_buffer_pointer);assert(depth>0);}
    if(close_before_release && close_calls==fail_close)nlr_raise("close failure");
    // Some stream close implementations release ownership and then raise.
    s->open=false;closes++;
    if(close_calls==fail_close)nlr_raise("close failure");
    return NULL;
}
static mp_uint_t stream_read(mp_obj_t obj,void *data,mp_uint_t n,int *err) {
    (void)err;struct stream *s=obj;assert(s->open);reads++;
    if(failing(reads,fail_read,"read callback failure"))return MP_STREAM_ERROR;
    n=OMV_MIN(n,sizeof(input)-s->pos);if(short_read && n)n--;
    memcpy(data,input+s->pos,n);s->pos+=n;return n;
}
static mp_uint_t stream_write(mp_obj_t obj,const void *data,mp_uint_t n,int *err) {
    (void)data;(void)err;struct stream *s=obj;assert(s->open);writes++;
    if(failing(writes,fail_write,"write callback failure"))return MP_STREAM_ERROR;
    s->pos+=n;return n;
}
static mp_uint_t stream_ioctl(mp_obj_t obj,mp_uint_t request,mp_uint_t arg,int *err) {
    (void)err;struct stream *s=obj;assert(s->open);
    if(request==MP_STREAM_FLUSH)return 0;
    assert(request==MP_STREAM_SEEK);seeks++;
    if(failing(seeks,fail_seek,"seek callback failure"))return MP_STREAM_ERROR;
    struct mp_stream_seek_t *seek=(void *)arg;
    seek->offset+=(seek->whence==SEEK_END?sizeof(input):seek->whence==SEEK_CUR?s->pos:0);
    assert(seek->offset>=0 && (size_t)seek->offset<=sizeof(input));s->pos=seek->offset;return 0;
}
static const mp_stream_p_t stream_api={stream_read,stream_write,stream_ioctl};
static const mp_stream_p_t *mp_get_stream_raise(mp_obj_t obj,int op){(void)obj;(void)op;return &stream_api;}
static void fb_alloc_mark(void){assert(depth<128);allocations[depth++]=(typeof(allocations[0])){.mark=true};}
static void *fb_alloc_all(uint32_t *size,int hint){
    (void)hint;if(fail_alloc)nlr_raise("allocation failure");
    *size=64;void *p=malloc(*size);assert(p);memset(p,0xa5,*size);
    allocations[depth++]=(typeof(allocations[0])){.ptr=p};return p;
}
static void fb_free(void){assert(depth);free(allocations[--depth].ptr);}
static void fb_free_buffer(void *ptr){for(int i=0;i<depth;i++)if(allocations[i].ptr==ptr){free(ptr);memmove(&allocations[i],&allocations[i+1],(--depth-i)*sizeof(allocations[0]));return;}assert(0);}
static void fb_alloc_free_till_mark(void){while(depth){bool mark=allocations[depth-1].mark;fb_free();if(mark)return;}}
#define IMLIB_IMAGE_MAX_SIZE(s) (s)
#define PIXFORMAT_JPEG 1
#define IM_IS_JPEG(i) ((i)->pixfmt==1)
#define IM_IS_BAYER(i) ((i)->pixfmt==2)
#define IM_IS_RGB565(i) ((i)->pixfmt==3)
#define IM_IS_GS(i) ((i)->pixfmt==4)
typedef struct {int pixfmt;uint32_t w,h,size;uint8_t *pixels;} image_t;
typedef struct {uint32_t jpg_w,jpg_h,jpg_size;} jpg_read_settings_t;
typedef enum {FORMAT_JPG,FORMAT_PNG,FORMAT_BMP,FORMAT_PNM,FORMAT_RAW,FORMAT_DONT_CARE} save_image_format_t;
typedef struct {int format,ppm_rs,bmp_rs,png_rs;jpg_read_settings_t jpg_rs;} img_read_settings_t;
static void image_alloc(image_t *i,size_t n){(void)i;(void)n;abort();}
static void ppm_read_geometry(file_t *fp,image_t *i,const char *p,int *r){(void)fp;(void)i;(void)p;(void)r;abort();}
static bool bmp_read_geometry(file_t *fp,image_t *i,const char *p,int *r){(void)fp;(void)i;(void)p;(void)r;abort();}
static void png_read_geometry(file_t *fp,image_t *i,const char *p,int *r){(void)fp;(void)i;(void)p;(void)r;abort();}
'''

CASES = r'''
static void assert_empty(void){
    assert(!callbacks && !depth && opens==closes);
    assert(!file_buffer_pointer && !file_buffer_offset && !file_buffer_size && !file_buffer_index);
}
static void load_geometry(const char *path){
    image_t image={0};
    CONSTRUCTOR_GEOMETRY
    assert(image.w==1 && image.h==1 && image.size==sizeof(input));
    uint8_t pixels[sizeof(input)];image.pixels=pixels;
    jpeg_read(&image,path);assert(!memcmp(pixels,input,sizeof(input)));
    nlr_pop_jump_callback(true);
}
static void fail_then_read(const char *expected,const char *path){
    nlr_buf_t nlr;
    if(!nlr_push(&nlr)){load_geometry(path);assert(0);}
    assert(!strcmp(nlr.ret_val,expected));assert_empty();
    fail_open=fail_read=fail_seek=fail_close=fail_alloc=0;short_read=check_abort_order=false;
    load_geometry("valid.jpg");assert_empty();
}
int main(int argc,char **argv){
    assert(argc>=2);
    if(!strcmp(argv[1],"mismatch")){
        for(int i=0;i<64;i++)fail_then_read("Image is not BMP!","jpeg.bmp");
    }else if(!strcmp(argv[1],"mismatch_close")){
        fail_close=2;check_abort_order=true;fail_then_read("Image is not BMP!","jpeg.bmp");
    }else if(!strcmp(argv[1],"open")){
        fail_open=atoi(argv[2]);fail_then_read("open failure","valid.jpg");
    }else if(!strcmp(argv[1],"read")){
        fail_read=atoi(argv[2]);direct_error=atoi(argv[3]);
        fail_then_read(direct_error?"read callback failure":"Read failed","valid.jpg");
    }else if(!strcmp(argv[1],"seek")){
        fail_seek=atoi(argv[2]);direct_error=atoi(argv[3]);
        fail_then_read(direct_error?"seek callback failure":"Seek failed","valid.jpg");
    }else if(!strcmp(argv[1],"short_read")){
        short_read=true;fail_then_read("Failed to read requested bytes!","valid.jpg");
    }else if(!strcmp(argv[1],"close")){
        fail_close=atoi(argv[2]);close_before_release=atoi(argv[3]);fail_then_read("close failure","valid.jpg");
    }else if(!strcmp(argv[1],"persistent_close_retry")){
        static file_t fp;file_open(&fp,"valid.jpg",false,FA_READ);
        fail_close=1;close_before_release=true;nlr_buf_t nlr;
        if(!nlr_push(&nlr)){file_close(&fp);assert(0);}
        assert(!strcmp(nlr.ret_val,"close failure") && fp.fp && opens==1 && closes==0 && !callbacks);
        file_close(&fp);assert(!fp.fp && close_calls==2);assert_empty();
    }else if(!strcmp(argv[1],"alloc")){
        fail_alloc=1;fail_then_read("allocation failure","valid.jpg");
    }else if(!strcmp(argv[1],"write")){
        nlr_buf_t nlr;fail_write=1;fail_close=1;direct_error=atoi(argv[2]);
        if(!nlr_push(&nlr)){
            py_image_alloc_cleanup_t cleanup;py_image_alloc_cleanup_begin(&cleanup,false);
            file_t fp;file_open(&fp,"out.jpg",true,FA_WRITE|FA_CREATE_ALWAYS);
            file_write(&fp,"abc",3);file_close(&fp);assert(0);
        }
        assert(!strcmp(nlr.ret_val,direct_error?"write callback failure":"Write failed"));assert_empty();
    }else if(!strcmp(argv[1],"independent_close")){
        py_image_alloc_cleanup_t cleanup;py_image_alloc_cleanup_begin(&cleanup,false);
        file_t buffered,other;file_open(&buffered,"a.jpg",true,FA_READ);
        void *saved=file_buffer_pointer;
        file_open(&other,"b.jpg",false,FA_READ);file_close(&other);
        assert(file_buffer_pointer==saved);
        unsigned char data[4];file_read(&buffered,data,4);assert(!memcmp(data,input,4));
        file_close(&buffered);nlr_pop_jump_callback(true);assert_empty();
    }else if(!strcmp(argv[1],"offset_and_non_top_buffer")){
        py_image_alloc_cleanup_t cleanup;py_image_alloc_cleanup_begin(&cleanup,false);
        file_t fp;file_open(&fp,"valid.jpg",false,FA_READ);file_seek(&fp,3);file_buffer_on(&fp);
        assert(file_buffer_offset==3);
        uint32_t size;void *later=fb_alloc_all(&size,0);memset(later,0x71,size);
        assert(depth==3);file_close(&fp);assert(depth==2 && !file_buffer_pointer);
        for(size_t i=0;i<size;i++)assert(((uint8_t *)later)[i]==0x71);
        nlr_pop_jump_callback(true);assert_empty();
    }else if(!strcmp(argv[1],"counts")){
        load_geometry("valid.jpg");assert_empty();printf("%d %d %d\n",reads,seeks,opens);return 0;
    }else{abort();}
    printf("PASS %s\n",argv[1]);return 0;
}
'''


def cleanup_code(source):
    start = source.index('typedef struct {\n    nlr_jump_callback_node_t callback;')
    end = source.index('static void py_image_alloc_cleanup(', start)
    return source[start:end] + '\n'.join(function(source, sig) for sig in (
        'static void py_image_alloc_cleanup(', 'static void py_image_alloc_cleanup_begin('))


class ImageIoLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler unavailable')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        path = Path(cls.directory.name)
        for header in ('py/runtime.h', 'py/nlr.h', 'py/obj.h', 'py/stream.h', 'py/builtin.h',
                       'extmod/vfs.h', 'omv_common.h', 'fb_alloc.h'):
            dest = path / header
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text('#include <runtime.h>\n')
        (path / 'runtime.h').write_text(RUNTIME)
        (path / 'imlib_config.h').write_text('#define IMLIB_ENABLE_IMAGE_FILE_IO 1\n')
        # Include the full compat implementation unchanged, not a replica of its
        # cleanup logic. Geometry and callback helpers are extracted verbatim.
        parts = ['#include "runtime.h"', '#include "' + str(ROOT / 'components/imlib/compat/file_utils.c') + '"',
                 SHIMS.replace('INPUT_BYTES', ','.join(map(str, BASELINE))),
                 cleanup_code((ROOT / 'modules/py_image.c').read_text())]
        parts.extend(function((ROOT / 'components/imlib/upstream/jpege.c').read_text(), sig)
                     for sig in ('void jpeg_read_geometry(', 'void jpeg_read_pixels(', 'void jpeg_read('))
        imlib = (ROOT / 'components/imlib/upstream/imlib.c').read_text()
        parts.extend(function(imlib, sig) for sig in ('static save_image_format_t imblib_parse_extension(',
                                                     'bool imlib_read_geometry('))
        constructor = function((ROOT / 'modules/py_image.c').read_text(), 'mp_obj_t py_image_make_new(')
        geometry = constructor[constructor.index('        file_t fp'):constructor.index('        image_alloc(&image')]
        geometry = geometry.replace('        const char *path = mp_obj_str_get_str(args[ARG_arg].u_obj);', '')
        parts.append(CASES.replace('CONSTRUCTOR_GEOMETRY', geometry))
        (path / 'test.c').write_text('\n'.join(parts))
        cls.executable = path / 'test'
        flags = os.environ.get('IMAGE_LIFETIME_CFLAGS', '').split()
        result = subprocess.run([compiler, '-std=gnu11', '-O0', '-g', '-Wall', '-Wextra',
                                 '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                                 '-I' + str(path), '-I' + str(ROOT / 'components/imlib/include'),
                                 *flags, str(path / 'test.c'), '-o', str(cls.executable)], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stderr)
        counts = subprocess.run([str(cls.executable), 'counts'], capture_output=True, text=True, check=True, timeout=30)
        cls.read_count, cls.seek_count, cls.open_count = map(int, counts.stdout.split())

    def run_case(self, *args):
        result = subprocess.run([str(self.executable), *map(str, args)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS ' + str(args[0]), result.stdout)

    def test_repeated_mismatch_followed_by_valid_jpeg(self):
        self.run_case('mismatch')

    def test_close_during_unwind_preserves_original_exception(self):
        self.run_case('mismatch_close')

    def test_each_open_failure(self):
        for number in range(1, self.open_count + 1):
            with self.subTest(number=number):
                self.run_case('open', number)

    def test_every_read_failure_as_status_and_python_exception(self):
        for direct in (0, 1):
            for number in range(1, self.read_count + 1):
                with self.subTest(number=number, direct=direct):
                    self.run_case('read', number, direct)

    def test_every_seek_failure_as_status_and_python_exception(self):
        for direct in (0, 1):
            for number in range(1, self.seek_count + 1):
                with self.subTest(number=number, direct=direct):
                    self.run_case('seek', number, direct)

    def test_short_read(self):
        self.run_case('short_read')

    def test_normal_close_failure(self):
        for number in range(1, self.open_count + 1):
            for before in (0, 1):
                with self.subTest(number=number, before_release=before):
                    self.run_case('close', number, before)

    def test_persistent_stream_close_failure_can_be_retried(self):
        self.run_case('persistent_close_retry')

    def test_allocation_failure_after_open(self):
        self.run_case('alloc')

    def test_buffered_write_failure_preserves_error_despite_close_failure(self):
        for direct in (0, 1):
            with self.subTest(direct=direct):
                self.run_case('write', direct)

    def test_unaligned_buffer_release_preserves_more_recent_allocation(self):
        self.run_case('offset_and_non_top_buffer')

    def test_closing_other_file_does_not_release_active_buffer(self):
        self.run_case('independent_close')


if __name__ == '__main__':
    unittest.main()

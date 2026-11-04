# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise production framebuffer bookkeeping against the pinned, real GC.

The collector deliberately receives no accidental register/stack roots: a C
static chain must remain valid even when conservative scanning finds no copy.
ESP heap calls are fault-injected; MicroPython's mark/sweep is not mocked.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

CONFIG = '''#include <stdint.h>
#include <alloca.h>
#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_MINIMUM)
#define MICROPY_ENABLE_GC (1)
#define MICROPY_GC_ALLOC_THRESHOLD (0)
#define MICROPY_ENABLE_FINALISER (0)
#define MICROPY_KBD_EXCEPTION (0)
#define MP_STATE_PORT MP_STATE_VM
typedef long mp_off_t;
'''
HEAP_HEADER = '''#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_SPIRAM 4
size_t heap_caps_get_largest_free_block(uint32_t caps);
void heap_caps_free(void *ptr);
'''
PRELUDE = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "py/gc.h"
#include "py/runtime.h"
#include "ev_memory.h"
mp_state_ctx_t mp_state_ctx;
const mp_obj_type_t mp_type_MemoryError = {0};
static jmp_buf oom;
static bool expect_oom;
static struct {void *ptr;size_t size;bool live;} native[128];
static size_t native_calls,native_live,native_frees,fail_call,gc_calls;
static _Alignas(32) uint8_t python_heap[16384];
NORETURN void mp_raise_msg(const mp_obj_type_t *type, mp_rom_error_text_t msg) {
    assert(type==&mp_type_MemoryError && msg!=NULL && expect_oom);
    longjmp(oom,1);
}
void *m_malloc(size_t n) {void *p=gc_alloc(n,0);assert(p);return p;}
void m_free(void *ptr) {gc_free(ptr);}
// Real GC root tracing and sweep. Omitting incidental stack references is a
// valid worst case, not a simulated reclamation of bookkeeping nodes.
void gc_collect(void) {gc_calls++;gc_collect_start();gc_collect_end();}
size_t heap_caps_get_largest_free_block(uint32_t caps) {(void)caps;return 131072;}
void *esp_vision_memory_malloc(size_t n,uint32_t caps) {
    assert(caps==MALLOC_CAP_8BIT);native_calls++;
    if(native_calls==fail_call)return NULL;
    for(size_t i=0;i<128;i++)if(!native[i].live){
        void *p=malloc(n);assert(p);memset(p,0xa5,n);
        native[i].ptr=p;native[i].size=n;native[i].live=true;native_live++;return p;
    }
    abort();
}
void *esp_vision_memory_malloc_preferred(size_t n,uint32_t caps,esp_vision_memory_preference_t preference) {
    assert(preference==ESP_VISION_MEMORY_PREFER_INTERNAL || preference==ESP_VISION_MEMORY_PREFER_SPIRAM);
    return esp_vision_memory_malloc(n,caps);
}
void heap_caps_free(void *ptr) {
    if(!ptr)return;
    for(size_t i=0;i<128;i++)if(native[i].live && native[i].ptr==ptr){
        memset(ptr,0xdd,native[i].size);free(ptr);native[i].live=false;native_live--;native_frees++;return;
    }
    fputs("invalid or duplicate native free\n",stderr);abort();
}
'''
CASES = r'''
static void callback_gc_and_churn(void) {
    gc_collect();
    // Python RAM-block callbacks can allocate objects after collection, reusing
    // the former bookkeeping slots. Force this with the actual GC allocator.
    for(size_t i=0;i<64;i++){void *p=gc_alloc(64,0);assert(p);memset(p,0x71,64);}
    gc_collect();
}
static void check_empty(void) {assert(!omv_fb_alloc_head && !native_live);}
int main(int argc,char **argv) {
    assert(argc==2);gc_init(python_heap,python_heap+sizeof(python_heap));
    if(!strcmp(argv[1],"baseline_probe")) {
        fb_alloc_mark();fb_alloc(257,FB_ALLOC_NO_HINT);
        size_t before=gc_nbytes(omv_fb_alloc_head);gc_collect();
        printf("head_gc_bytes_before=%zu after=%zu native_live=%zu\n",before,gc_nbytes(omv_fb_alloc_head),native_live);
        return 0; // Do not dereference a reclaimed baseline node.
    } else if(!strcmp(argv[1],"live_callback")) {
        fb_alloc_mark();uint8_t *p=fb_alloc(257,FB_ALLOC_NO_HINT);memset(p,0x5c,257);
        callback_gc_and_churn();for(size_t i=0;i<257;i++)assert(p[i]==0x5c);
        fb_free();assert(native_live==1);fb_alloc_free_till_mark();check_empty();assert(native_frees==3);
    } else if(!strcmp(argv[1],"nested_marks")) {
        fb_alloc_mark();fb_alloc(64,FB_ALLOC_PREFER_INTERNAL);fb_alloc_mark();fb_alloc(32,FB_ALLOC_PREFER_SIZE);
        callback_gc_and_churn();fb_alloc_free_till_mark();assert(native_live==3);
        callback_gc_and_churn();fb_alloc_free_till_mark();check_empty();assert(native_frees==6);
    } else if(!strcmp(argv[1],"permanent_mark")) {
        fb_alloc_mark();fb_alloc_mark();fb_alloc_mark_permanent();fb_alloc(32,FB_ALLOC_NO_HINT);
        callback_gc_and_churn();fb_alloc_free_till_mark();assert(native_live==2);
        fb_alloc_free_till_mark();assert(native_live==2);
        fb_alloc_free_till_mark_past_mark_permanent();assert(native_live==1);
        fb_free_all();check_empty();assert(native_frees==4);
    } else if(!strcmp(argv[1],"imageio_permanent")) {
        // Actual ImageIO constructor order: permanence belongs to the data
        // node, not to the preceding scope mark.
        fb_alloc_mark();uint8_t *p=fb_alloc(64,FB_ALLOC_PREFER_SIZE|FB_ALLOC_CACHE_ALIGN);
        memset(p,0x3d,64);fb_alloc_mark_permanent();callback_gc_and_churn();
        fb_alloc_free_till_mark();fb_alloc_free_till_mark();
        assert(native_live==3 && !native_frees && omv_fb_alloc_head);
        for(size_t i=0;i<64;i++)assert(p[i]==0x3d);
        fb_alloc_free_till_mark_past_mark_permanent();check_empty();assert(native_frees==3);
    } else if(!strcmp(argv[1],"imageio_nested")) {
        fb_alloc_mark();fb_alloc(17,FB_ALLOC_NO_HINT);
        fb_alloc_mark();uint8_t *p=fb_alloc(64,FB_ALLOC_PREFER_SIZE|FB_ALLOC_CACHE_ALIGN);
        memset(p,0x56,64);fb_alloc_mark_permanent();
        fb_alloc_mark();fb_alloc(32,FB_ALLOC_NO_HINT);
        fb_alloc_mark();fb_alloc(48,FB_ALLOC_NO_HINT);callback_gc_and_churn();
        fb_alloc_free_till_mark();assert(native_live==9);
        fb_alloc_free_till_mark();assert(native_live==6);
        fb_alloc_free_till_mark();fb_alloc_free_till_mark();assert(native_live==6 && native_frees==6);
        for(size_t i=0;i<64;i++)assert(p[i]==0x56);
        fb_alloc_free_till_mark_past_mark_permanent();assert(native_live==3 && native_frees==9);
        fb_alloc_free_till_mark();check_empty();assert(native_frees==12);
    } else if(!strcmp(argv[1],"imageio_reset")) {
        for(size_t pass=0;pass<2;pass++){
            fb_alloc_mark();fb_alloc(64,FB_ALLOC_PREFER_SIZE|FB_ALLOC_CACHE_ALIGN);
            fb_alloc_mark_permanent();callback_gc_and_churn();
            fb_alloc_free_till_mark();fb_alloc_free_till_mark();assert(native_live==3);
            if(pass)fb_alloc_init0();else fb_free_all();
            fb_free_all();fb_alloc_init0();check_empty();assert(native_frees==3*(pass+1));
        }
    } else if(!strcmp(argv[1],"all_alignment")) {
        uint32_t size=0;uint8_t *p=fb_alloc_all(&size,FB_ALLOC_PREFER_SIZE);
        assert(size==98304 && ((uintptr_t)p%OMV_ALLOC_ALIGNMENT)==0);
        memset(p,0x68,size);callback_gc_and_churn();assert(p[0]==0x68 && p[size-1]==0x68);
        fb_free();check_empty();assert(native_frees==2);
    } else if(!strcmp(argv[1],"node_oom")) {
        fb_alloc_mark();omv_fb_alloc_node_t *saved=omv_fb_alloc_head;
        fail_call=native_calls+1;expect_oom=true;
        if(!setjmp(oom)){fb_alloc(256,FB_ALLOC_NO_HINT);abort();}
        expect_oom=false;assert(omv_fb_alloc_head==saved && native_live==1 && !native_frees);
        fail_call=native_calls+1;expect_oom=true;
        if(!setjmp(oom)){fb_alloc_mark();abort();}
        expect_oom=false;assert(omv_fb_alloc_head==saved && native_live==1 && !native_frees);
        fb_free_all();check_empty();
    } else if(!strcmp(argv[1],"raw_oom")) {
        fb_alloc_mark();omv_fb_alloc_node_t *saved=omv_fb_alloc_head;
        fail_call=native_calls+2;expect_oom=true;
        if(!setjmp(oom)){fb_alloc(256,FB_ALLOC_NO_HINT);abort();}
        expect_oom=false;assert(omv_fb_alloc_head==saved && native_live==1 && native_frees==1);
        callback_gc_and_churn();fb_free_all();check_empty();assert(native_frees==2);
    } else if(!strcmp(argv[1],"gc_rebuild")) {
        fb_alloc_mark();fb_alloc(257,FB_ALLOC_NO_HINT);callback_gc_and_churn();
        // A new VM lifetime clears/reinitializes MP state and may reuse all of
        // the old GC heap before fb_alloc_init0, as at firmware startup.
        memset(&mp_state_ctx,0,sizeof(mp_state_ctx));memset(python_heap,0x89,sizeof(python_heap));
        gc_init(python_heap,python_heap+sizeof(python_heap));
        fb_alloc_init0();check_empty();assert(native_frees==3);
        fb_alloc_mark();fb_alloc(11,FB_ALLOC_NO_HINT);fb_alloc_init0();fb_alloc_init0();check_empty();
        assert(native_frees==6);
    } else if(!strcmp(argv[1],"zero")) {
        assert(!fb_alloc(0,FB_ALLOC_NO_HINT));fb_free();fb_free_all();fb_alloc_init0();check_empty();assert(!native_calls);
    } else if(!strcmp(argv[1],"real_gc_control")) {
        // Positive control: the pinned collector follows prev pointers when a
        // genuine root is provided, then reclaims both nodes without that root.
        void **child=gc_alloc(64,0);void **head=gc_alloc(64,0);assert(child && head);head[0]=child;
        gc_collect_start();gc_collect_root((void **)&head,1);gc_collect_end();
        assert(gc_nbytes(head)==64 && gc_nbytes(child)==64);
        gc_collect();assert(!gc_nbytes(head) && !gc_nbytes(child));
    } else {abort();}
    printf("PASS %s native_allocations=%zu frees=%zu real_gc_cycles=%zu\n",argv[1],native_calls,native_frees,gc_calls);
    return 0;
}
'''


class FramebufferAllocationLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler unavailable')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        (directory / 'genhdr').mkdir()
        (directory / 'mpconfigport.h').write_text(CONFIG)
        (directory / 'genhdr/qstrdefs.generated.h').write_text('QDEF0(MP_QSTRnull, 0, 0, "")\n')
        (directory / 'genhdr/root_pointers.h').write_text('')
        (directory / 'esp_heap_caps.h').write_text(HEAP_HEADER)
        (directory / 'omv_common.h').write_text('#pragma once\n#define OMV_CACHE_LINE_SIZE 32\n'
                                               '#define OMV_ALIGN_TO(v,a) (((v)+(a)-1)&~((a)-1))\n')
        # Compile the entire production implementation and pinned MicroPython
        # gc.c unchanged. Only host config and native heap/error hooks are shims.
        (directory / 'lifetime.c').write_text(PRELUDE + (ROOT / 'components/imlib/compat/fb_alloc.c').read_text() + CASES)
        cls.executable = directory / 'lifetime'
        result = subprocess.run([compiler, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-unused-parameter', '-ffunction-sections', '-fdata-sections',
                                 '-I' + str(directory), '-I' + str(ROOT / 'lib/micropython'),
                                 '-I' + str(ROOT / 'platform'), '-I' + str(ROOT / 'components/imlib/include'),
                                 str(directory / 'lifetime.c'), str(ROOT / 'lib/micropython/py/gc.c'),
                                 '-Wl,--gc-sections', '-o', str(cls.executable)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS ' + name, result.stdout)

    def test_callback_collection_and_churn_keep_live_buffers(self):
        self.run_case('live_callback')

    def test_nested_marks_release_only_the_inner_scope(self):
        self.run_case('nested_marks')

    def test_permanent_marks_and_explicit_release(self):
        self.run_case('permanent_mark')

    def test_imageio_permanent_data_survives_repeated_normal_cleanup(self):
        self.run_case('imageio_permanent')

    def test_imageio_permanent_data_survives_nested_temporary_scopes(self):
        self.run_case('imageio_nested')

    def test_imageio_permanent_data_is_released_once_by_free_all_and_reset(self):
        self.run_case('imageio_reset')

    def test_alloc_all_alignment_and_ownership(self):
        self.run_case('all_alignment')

    def test_node_oom_preserves_existing_chain(self):
        self.run_case('node_oom')

    def test_raw_oom_releases_unlinked_node(self):
        self.run_case('raw_oom')

    def test_gc_rebuild_and_repeated_vm_cleanup(self):
        self.run_case('gc_rebuild')

    def test_zero_size_and_empty_cleanup(self):
        self.run_case('zero')

    def test_real_gc_traces_registered_roots_and_reclaims_unrooted_nodes(self):
        self.run_case('real_gc_control')


if __name__ == '__main__':
    unittest.main()

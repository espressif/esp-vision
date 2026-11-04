# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Fault-inject the production UVC transfer callback and pinned class functions."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_jpeg_validation import BASELINE, function

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).with_name('tinyusb_video_stubs') / 'upstream_transfer.c'
INCLUDE = ROOT / 'overlay/micropython/shared/tinyusb'
spec = importlib.util.spec_from_file_location('prepare_tinyusb_video', ROOT / 'tools/prepare_tinyusb_video.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CFG_TUD_VIDEO 1
#define CFG_TUD_VIDEO_STREAMING 1
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE 512
#define TU_ATTR_PACKED
#define TU_ATTR_WEAK __attribute__((weak))
#define TU_ASSERT(c,...) do {if(!(c)) return false;} while(0)
#define TU_VERIFY(c,...) TU_ASSERT(c)
#define TU_LOG_DRV(...)
#define TU_ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define TUD_EPBUF_DEF(n,size) uint8_t n[size]
#define tu_memclr(p,n) memset(p,0,n)
#define VS_STATE_PROBING 0
#define VS_STATE_COMMITTED 1
#define VS_STATE_STREAMING 2
#define TUSB_XFER_BULK 2
#define TUSB_XFER_ISOCHRONOUS 1
#define TUD_OPT_RHPORT 0
#define USBD_UVC_EP_IN 0x85
#define ESP_VISION_UVC_INTERVAL_US 100000
#define ESP_VISION_UVC_MAX_FRAME (48*1024)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_OK 0
#define ESP_ERR_INVALID_SIZE 1
#define ESP_ERR_NO_MEM 2
typedef int esp_err_t;
typedef enum {XFER_RESULT_SUCCESS=0,XFER_RESULT_FAILED=1} xfer_result_t;
typedef struct {uint32_t unused;} video_probe_and_commit_control_t;
typedef struct {uint8_t bHeaderLength;union {uint8_t bmHeaderInfo;struct {
 uint8_t FrameID:1,EndOfFrame:1,PresentationTime:1,SourceClockReference:1,
 Reserved:1,StillImage:1,Error:1,EndOfHeader:1;};};} tusb_video_payload_header_t;
typedef struct {uint8_t bLength,bDescriptorType,bEndpointAddress;struct {uint8_t xfer;} bmAttributes;uint16_t wMaxPacketSize;} tusb_desc_endpoint_t;
typedef struct {uint8_t bLength,bDescriptorType,bInterfaceNumber,bAlternateSetting,bNumEndpoints;} tusb_desc_interface_t;
static uint8_t descriptor[64];
static bool mounted=true,suspended,endpoint_busy,claim_ok=true;
static unsigned queued_count,complete_count,failure_count,fail_queue_at;
static uint16_t lengths[256];static uint8_t packets[256][512];
static int64_t now_us=1000000;
static int lock_depth;
static bool tud_mounted(void){return mounted;}
static bool tud_suspended(void){return suspended;}
static void mp_usbd_lock(void){lock_depth++;}
static void mp_usbd_unlock(void){assert(lock_depth>0);lock_depth--;}
static void *heap_caps_malloc(size_t n,int caps){(void)caps;return malloc(n);}
static int64_t esp_timer_get_time(void){return now_us;}
static bool usbd_edpt_busy(uint8_t r,uint8_t ep){(void)r;(void)ep;return endpoint_busy;}
static bool usbd_edpt_claim(uint8_t r,uint8_t ep){(void)r;assert(ep==0x85);return claim_ok;}
static bool usbd_edpt_xfer(uint8_t r,uint8_t ep,uint8_t *data,uint16_t size){
 (void)r;assert(ep==0x85);assert(size>=2&&size<=512);assert(queued_count<256);
 if(fail_queue_at==queued_count+1){fail_queue_at=0;return false;}
 lengths[queued_count]=size;memcpy(packets[queued_count++],data,size);endpoint_busy=true;return true;
}
static void usbd_edpt_close(uint8_t r,uint8_t ep){(void)r;(void)ep;endpoint_busy=false;}
static bool usbd_edpt_open(uint8_t r,const tusb_desc_endpoint_t *ep){(void)r;(void)ep;return true;}
static uint8_t _desc_itfnum(const void *p){return ((const uint8_t*)p)[2];}
static const uint8_t *_find_desc_itf(const uint8_t *b,const uint8_t *e,uint8_t n,uint8_t alt){
 (void)e;(void)n;(void)alt;return b;
}
static const uint8_t *_find_desc_ep(const uint8_t *b,const uint8_t *e){(void)e;return b;}
static const uint8_t *tu_desc_next(const void *p){return (const uint8_t*)p+((const uint8_t*)p)[0];}
static unsigned tu_edpt_packet_size(const tusb_desc_endpoint_t *p){return p->wMaxPacketSize;}
struct unused_stm;
// The alt-zero test retains committed state, so parameter negotiation is unused.
#define _init_vs_configuration(s) true
void tud_video_frame_xfer_complete_cb(uint_fast8_t,uint_fast8_t);
void tud_video_frame_xfer_failed_cb(uint_fast8_t,uint_fast8_t);
#include "adapted_transfer.c"
static uint8_t *s_frames[2];static size_t s_sizes[2];
static int s_latest=-1,s_inflight=-1;
static bool s_busy,s_preview,s_stream_started=true;
static int64_t s_next_frame_us;
static const uint8_t esp_vision_uvc_logo[]={0xff,0xd8,0xff,0xd9};
#define tud_video_frame_xfer_complete_cb original_complete
COMPLETE_CALLBACK
#undef tud_video_frame_xfer_complete_cb
#define tud_video_frame_xfer_failed_cb original_failed
FAILED_CALLBACK
#undef tud_video_frame_xfer_failed_cb
void tud_video_frame_xfer_complete_cb(uint_fast8_t c,uint_fast8_t s){complete_count++;original_complete(c,s);}
void tud_video_frame_xfer_failed_cb(uint_fast8_t c,uint_fast8_t s){failure_count++;original_failed(c,s);}
PUBLISH_FUNCTION
POLL_FUNCTION

static void setup(void){
 memset(_videod_itf,0,sizeof(_videod_itf));memset(_videod_streaming_itf,0,sizeof(_videod_streaming_itf));
 memset(_videod_streaming_epbuf,0,sizeof(_videod_streaming_epbuf));
 _videod_itf[0].beg=descriptor;descriptor[10]=0x85;_videod_itf[0].stm[0]=0;
 _videod_streaming_itf[0].desc.beg=1;_videod_streaming_itf[0].desc.ep[0]=8;
 _videod_streaming_itf[0].desc.end=64;_videod_streaming_itf[0].state=VS_STATE_STREAMING;
 _videod_streaming_itf[0].max_payload_transfer_size=512;
 _videod_streaming_epbuf[0].buf[0]=2;
}
static void complete_packet(xfer_result_t result,uint32_t size){
 endpoint_busy=false;assert(videod_xfer_cb(0,0x85,result,size));
}
static void run_good_frame(const uint8_t *jpeg,size_t bytes,const char *output){
 unsigned start=queued_count,previous_complete=complete_count;
 uint8_t old_fid=_videod_streaming_epbuf[0].buf[1]&1;
 assert(tud_video_n_frame_xfer(0,0,(void*)jpeg,bytes));
 unsigned index=start;
 while(_videod_streaming_itf[0].buffer){assert(index<queued_count);complete_packet(XFER_RESULT_SUCCESS,lengths[index++]);}
 assert(complete_count==previous_complete+1);assert(s_inflight==-1&&!s_busy);
 FILE *f=fopen(output,"wb");assert(f);size_t got=0;
 for(unsigned i=start;i<queued_count;i++){
  assert(packets[i][0]==2);assert((packets[i][1]&1)==(old_fid^1));assert(!(packets[i][1]&0x40));
  assert(!!(packets[i][1]&2)==(i==queued_count-1));
  assert(!memcmp(packets[i]+2,jpeg+got,lengths[i]-2));got+=lengths[i]-2;
  assert(fwrite(packets[i]+2,1,lengths[i]-2,f)==(size_t)lengths[i]-2);
 }
 fclose(f);assert(got==bytes);
}
int main(int argc,char **argv){
 assert(argc==4);unsigned scenario=(unsigned)atoi(argv[1]);
 FILE *f=fopen(argv[2],"rb");assert(f);uint8_t jpeg[8192];size_t size=fread(jpeg,1,sizeof(jpeg),f);fclose(f);assert(size>1530);
 setup();
 if(scenario==0){run_good_frame(jpeg,size,argv[3]);puts("ok");return 0;}
 bool accepted=false;assert(esp_vision_uvc_publish(jpeg,size,true,&accepted)==ESP_OK&&accepted);
 if(scenario==10){
  fail_queue_at=1;esp_vision_uvc_poll();assert(!queued_count&&!s_busy&&s_inflight==-1);
  assert(failure_count==1&&complete_count==0&&!_videod_streaming_itf[0].buffer);
  run_good_frame(jpeg,size,argv[3]);free(s_frames[0]);puts("ok");return 0;
 }
 if(scenario==14){
  uint8_t before[512];memcpy(before,_videod_streaming_epbuf[0].buf,512);
  claim_ok=false;esp_vision_uvc_poll();claim_ok=true;
  assert(!queued_count&&!s_busy&&s_inflight==-1&&!complete_count&&!failure_count);
  assert(!_videod_streaming_itf[0].buffer&&!_videod_streaming_itf[0].offset);
  assert(!memcmp(before,_videod_streaming_epbuf[0].buf,512));
  run_good_frame(jpeg,size,argv[3]);free(s_frames[0]);puts("ok");return 0;
 }
 esp_vision_uvc_poll();assert(s_inflight==0&&s_busy&&queued_count==1);
 uint8_t original[8192];memcpy(original,s_frames[0],size);
 uint8_t replacement[8192];memset(replacement,0xa7,size);
 assert(esp_vision_uvc_publish(replacement,size,true,&accepted)==ESP_OK&&accepted);
 assert(s_latest==1&&s_inflight==0&&!memcmp(s_frames[0],original,size));
 unsigned expected_failures=1;
 if(scenario==7){ // Real class bus-reset cancellation: no completion success.
  endpoint_busy=false;videod_reset(0);assert(failure_count==1&&!s_busy&&s_inflight==-1);
  setup();
 }else if(scenario==8){ // Real alt-zero cancellation and source clearing.
  _videod_streaming_itf[0].state=VS_STATE_COMMITTED;
  descriptor[1]=5;descriptor[5]=0; // alternate interface with zero endpoints
  assert(_open_vs_itf(0,&_videod_streaming_itf[0],0));
  assert(failure_count==1&&!_videod_streaming_itf[0].buffer&&!s_busy&&s_inflight==-1);
  setup();
 }else{
  complete_packet(XFER_RESULT_SUCCESS,lengths[0]);assert(queued_count==2);
  if(scenario==5||scenario==15){
   uint8_t before[512];memcpy(before,_videod_streaming_epbuf[0].buf,512);
   claim_ok=false;complete_packet(scenario==5?XFER_RESULT_FAILED:XFER_RESULT_SUCCESS,scenario==5?0:lengths[1]);claim_ok=true;
   assert(!memcmp(before,_videod_streaming_epbuf[0].buf,512));
  }
  else if(scenario==6){fail_queue_at=queued_count+1;complete_packet(XFER_RESULT_FAILED,0);}
  else{
   unsigned failed_index=1;
   if(scenario==4){while(_videod_streaming_itf[0].offset<size){complete_packet(XFER_RESULT_SUCCESS,lengths[failed_index++]);}}
   if(scenario==3){fail_queue_at=queued_count+1;complete_packet(XFER_RESULT_SUCCESS,lengths[failed_index]);}
   else complete_packet(scenario==1||scenario==4?XFER_RESULT_FAILED:XFER_RESULT_SUCCESS,
      scenario==1||scenario==4||scenario==11?0:scenario==12?lengths[failed_index]+1:lengths[failed_index]-1);
   assert(_videod_streaming_itf[0].failed);assert(complete_count==0&&failure_count==0);
   assert(queued_count==failed_index+2);assert(lengths[queued_count-1]==2);
   assert((packets[queued_count-1][1]&0x42)==0x42); // Same FID plus ERR/EOF.
   assert((packets[queued_count-1][1]&1)==(packets[0][1]&1));
   assert(s_inflight==0&&s_busy); // No source slot reuse until terminator completes.
   assert(esp_vision_uvc_publish(replacement,size,true,&accepted)==ESP_OK);
   assert(!memcmp(s_frames[0],original,size));
   unsigned before=queued_count;complete_packet(scenario==9?XFER_RESULT_FAILED:XFER_RESULT_SUCCESS,scenario==9?0:scenario==13?1:2);
   assert(queued_count==before); // Error terminator is never retried.
  }
  assert(failure_count==expected_failures&&complete_count==0&&!s_busy&&s_inflight==-1);
  assert(!_videod_streaming_itf[0].buffer);
  // A duplicate callback after release cannot free a new source or report success.
  complete_packet(XFER_RESULT_SUCCESS,2);assert(failure_count==expected_failures&&complete_count==0);
 }
 assert(lock_depth==0);run_good_frame(jpeg,size,argv[3]);assert(failure_count==expected_failures);
 free(s_frames[0]);free(s_frames[1]);puts("ok");return 0;
}
'''


class TinyUsbVideoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler unavailable')
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        upstream = FIXTURE.read_text()
        (cls.directory / 'adapted_transfer.c').write_text(adapter.patch_source(upstream))
        uvc = (ROOT / 'platform/uvc.c').read_text()
        harness = HARNESS
        for marker, signature in [
            ('COMPLETE_CALLBACK', 'void tud_video_frame_xfer_complete_cb('),
            ('FAILED_CALLBACK', 'void tud_video_frame_xfer_failed_cb('),
            ('PUBLISH_FUNCTION', 'esp_err_t esp_vision_uvc_publish('),
            ('POLL_FUNCTION', 'static void esp_vision_uvc_poll('),
        ]:
            harness = harness.replace(marker, function(uvc, signature))
        (cls.directory / 'harness.c').write_text(harness)
        cls.binary = cls.directory / 'test-transfer'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-address',
                        '-Wno-unused-function', '-I', str(cls.directory), '-I', str(INCLUDE),
                        str(cls.directory / 'harness.c'), '-o', str(cls.binary)], check=True, capture_output=True)
        # Insert a legal COM segment into the self-generated JPEG so it spans
        # several ISO payloads. Complete recovery output must equal this source.
        cls.jpeg = BASELINE[:2] + b'\xff\xfe' + (2602).to_bytes(2, 'big') + bytes(2600) + BASELINE[2:]
        cls.input = cls.directory / 'source.jpg'
        cls.input.write_bytes(cls.jpeg)

    def scenario(self, number):
        output = self.directory / f'output-{number}.jpg'
        subprocess.run([str(self.binary), str(number), str(self.input), str(output)], check=True, capture_output=True)
        self.assertEqual(output.read_bytes(), self.jpeg)

    def test_complete_frame_byte_identity_and_eof(self): self.scenario(0)
    def test_failed_zero_bytes_aborts_and_next_frame_recovers(self): self.scenario(1)
    def test_short_success_aborts_without_advancing(self): self.scenario(2)
    def test_next_payload_queue_failure_terminates(self): self.scenario(3)
    def test_final_payload_failure_is_not_complete(self): self.scenario(4)
    def test_terminator_claim_failure_releases_once(self): self.scenario(5)
    def test_terminator_queue_failure_releases_once(self): self.scenario(6)
    def test_bus_reset_cancels_source_and_releases_slot(self): self.scenario(7)
    def test_alt_zero_cancels_source_and_releases_slot(self): self.scenario(8)
    def test_failed_terminator_never_retries_or_succeeds(self): self.scenario(9)
    def test_initial_queue_failure_releases_class_source(self): self.scenario(10)
    def test_zero_byte_success_aborts(self): self.scenario(11)
    def test_oversized_completion_aborts(self): self.scenario(12)
    def test_short_error_terminator_releases_once(self): self.scenario(13)
    def test_initial_busy_claim_preserves_fid_and_endpoint_bytes(self): self.scenario(14)
    def test_next_payload_busy_claim_aborts_without_endpoint_write(self): self.scenario(15)

    def test_unknown_upstream_source_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Unsupported TinyUSB'):
            adapter.prepare(FIXTURE.read_text())

    def test_fixture_matches_installed_pinned_source_when_available(self):
        # The mandatory tests above do not depend on managed-component caches.
        # When supplied, verify every retained upstream function byte for byte
        # and generate the full production translation unit used by the build.
        path = os.environ.get('ESP_VISION_TINYUSB_VIDEO_SOURCE')
        if not path:
            return
        upstream = Path(path).read_text()
        self.assertEqual(hashlib.sha256(upstream.encode()).hexdigest(), adapter.SOURCE_SHA256)
        fixture = FIXTURE.read_text()
        for signature in ['static uint_fast16_t _prepare_in_payload(', 'bool tud_video_n_frame_xfer(',
                          'void videod_reset(', 'static bool _open_vs_itf(', 'bool videod_xfer_cb(']:
            self.assertEqual(function(fixture, signature), function(upstream, signature))
        generated = adapter.prepare(upstream)
        self.assertTrue(generated.startswith(upstream[:upstream.index('#include')]))
        self.assertEqual(generated.count('#include "esp_vision_video_transfer.inc"'), 1)


if __name__ == '__main__':
    unittest.main()

/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Koji KITAYAMA
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

// ESP-VISION adaptation of videod_xfer_cb from espressif/tinyusb 0.19.0~3
// (ab4a1817907ed865b10a712b5e03e5e0a9902df5). Preserve complete frame boundaries
// after an ISO error; an ERR/EOF terminator never reports successful completion.

static void esp_vision_video_release(videod_streaming_interface_t *stm, bool failed) {
  if (!stm->buffer) return;
  stm->buffer = NULL;
  stm->bufsize = 0;
  stm->offset = 0;
  stm->queued_size = 0;
  stm->failed = false;
  if (failed) {
    if (tud_video_frame_xfer_failed_cb) {
      tud_video_frame_xfer_failed_cb(stm->index_vc, stm->index_vs);
    }
  } else {
    tud_video_frame_xfer_complete_cb(stm->index_vc, stm->index_vs);
  }
}

static void esp_vision_video_fail(uint8_t rhport, uint8_t ep_addr,
                                  videod_streaming_interface_t *stm,
                                  videod_streaming_epbuf_t *epbuf) {
  // Discard all remaining JPEG bytes. At most one header-only error terminator
  // retains the same FID, so a receiver can discard its partially received frame.
  if (!usbd_edpt_claim(rhport, ep_addr)) {
    esp_vision_video_release(stm, true);
    return;
  }
  tusb_video_payload_header_t *hdr = (tusb_video_payload_header_t *)epbuf->buf;
  stm->failed = true;
  hdr->Error = 1;
  hdr->EndOfFrame = 1;
  stm->queued_size = sizeof(*hdr);
  if (!usbd_edpt_xfer(rhport, ep_addr, epbuf->buf, stm->queued_size)) {
    esp_vision_video_release(stm, true);
  }
}

bool videod_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
  uint_fast8_t itf;
  videod_interface_t *ctl;
  videod_streaming_interface_t *stm;
  for (itf = 0; itf < CFG_TUD_VIDEO_STREAMING; ++itf) {
    stm = &_videod_streaming_itf[itf];
    uint_fast16_t const ep_ofs = stm->desc.ep[0];
    if (!ep_ofs) continue;
    ctl = &_videod_itf[stm->index_vc];
    uint8_t const *desc = ctl->beg;
    if (ep_addr == _desc_ep_addr(desc + ep_ofs)) break;
  }
  TU_ASSERT(itf < CFG_TUD_VIDEO_STREAMING);
  videod_streaming_epbuf_t *epbuf = &_videod_streaming_epbuf[itf];
  if (!stm->buffer) return true;
  if (stm->failed) {
    // Completion (or failure) of the terminator releases the source once.
    esp_vision_video_release(stm, true);
  } else if (result != XFER_RESULT_SUCCESS || xferred_bytes != stm->queued_size) {
    esp_vision_video_fail(rhport, ep_addr, stm, epbuf);
  } else if (stm->offset < stm->bufsize) {
    if (!usbd_edpt_claim(rhport, ep_addr)) {
      esp_vision_video_release(stm, true);
      return true;
    }
    uint_fast16_t pkt_len = _prepare_in_payload(stm, epbuf->buf);
    if (!usbd_edpt_xfer(rhport, ep_addr, epbuf->buf, (uint16_t)pkt_len)) {
      esp_vision_video_fail(rhport, ep_addr, stm, epbuf);
    }
  } else {
    esp_vision_video_release(stm, false);
  }
  return true;
}

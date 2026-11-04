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

// Verbatim transfer declarations/functions from espressif/tinyusb 0.19.0~3.
// Commit ab4a1817907ed865b10a712b5e03e5e0a9902df5; full video_device.c SHA256
// c3a99094c039c51de88ad06eb70807d2d645cbb552bc262577ce7b2404012315.
// The build adapter applies its same function edits to this host fixture.

typedef struct TU_ATTR_PACKED {
  uint8_t index_vc;  /* index of bound video control interface */
  uint8_t index_vs;  /* index from the video control interface */
  struct {
    uint16_t beg;    /* Offset of the begging of video streaming interface descriptor */
    uint16_t end;    /* Offset of the end of video streaming interface descriptor */
    uint16_t cur;    /* Offset of the current settings */
    uint16_t ep[2];  /* Offset of endpoint descriptors. 0: streaming, 1: still capture */
  } desc;
  uint8_t *buffer;   /* frame buffer. assume linear buffer. no support for stride access */
  uint32_t bufsize;  /* frame buffer size */
  uint32_t offset;   /* offset for the next payload transfer */
  uint32_t max_payload_transfer_size;
  uint8_t  error_code;/* error code */
  uint8_t  state;    /* 0:probing 1:committed 2:streaming */

  video_probe_and_commit_control_t probe_commit_payload; /* Probe and Commit control */
} videod_streaming_interface_t;

typedef struct {
  TUD_EPBUF_DEF(buf, CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE);
} videod_streaming_epbuf_t;

typedef struct TU_ATTR_PACKED {
  const uint8_t*beg;                     /* The head of the first video control interface descriptor */
  uint16_t len;                          /* Byte length of the descriptors */
  uint16_t cur;                          /* offset for current video control interface */
  uint8_t  stm[CFG_TUD_VIDEO_STREAMING]; /* Indices of streaming interface */
  uint8_t  error_code;                   /* error code */
  uint8_t  power_mode;
} videod_interface_t;

static videod_interface_t _videod_itf[CFG_TUD_VIDEO];
static videod_streaming_interface_t _videod_streaming_itf[CFG_TUD_VIDEO_STREAMING];
static videod_streaming_epbuf_t _videod_streaming_epbuf[CFG_TUD_VIDEO_STREAMING];

static inline uint8_t _desc_ep_addr(void const *desc) {
  return ((uint8_t const*)desc)[2];
}

static videod_streaming_interface_t* _get_instance_streaming(uint_fast8_t ctl_idx, uint_fast8_t stm_idx) {
  videod_interface_t *ctl = &_videod_itf[ctl_idx];
  if (!ctl->beg) return NULL;
  videod_streaming_interface_t *stm = &_videod_streaming_itf[ctl->stm[stm_idx]];
  if (!stm->desc.beg) return NULL;
  return stm;
}

/** Set the alternate setting to own video streaming interface. */
static bool _open_vs_itf(uint8_t rhport, videod_streaming_interface_t *stm, uint_fast8_t altnum)
{
  uint_fast8_t i;
  TU_LOG_DRV("    reopen VS %d\r\n", altnum);
  uint8_t const *desc = _videod_itf[stm->index_vc].beg;

#ifndef TUP_DCD_EDPT_ISO_ALLOC
  /* Close endpoints of previous settings. */
  for (i = 0; i < TU_ARRAY_SIZE(stm->desc.ep); ++i) {
    uint_fast16_t ofs_ep = stm->desc.ep[i];
    if (!ofs_ep) break;
    tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const*)(desc + ofs_ep);
    /* Only ISO endpoints needs to be closed */
    if(ep->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS) {
      stm->desc.ep[i] = 0;
      usbd_edpt_close(rhport, ep->bEndpointAddress);
      TU_LOG_DRV("    close EP%02x\r\n", ep->bEndpointAddress);
    }
  }
#endif

  /* clear transfer management information */
  stm->buffer  = NULL;
  stm->bufsize = 0;
  stm->offset  = 0;

  /* Find a alternate interface */
  uint8_t const *beg = desc + stm->desc.beg;
  uint8_t const *end = desc + stm->desc.end;
  uint8_t const *cur = _find_desc_itf(beg, end, _desc_itfnum(beg), altnum);
  TU_VERIFY(cur < end);

  uint_fast8_t numeps = ((tusb_desc_interface_t const *)cur)->bNumEndpoints;
  TU_ASSERT(numeps <= TU_ARRAY_SIZE(stm->desc.ep));
  stm->desc.cur = (uint16_t)(cur - desc); /* Save the offset of the new settings */
  if (!altnum && (VS_STATE_COMMITTED != stm->state)) {
    TU_VERIFY(_init_vs_configuration(stm));
  }
  /* Open bulk or isochronous endpoints of the new settings. */
  for (i = 0, cur = tu_desc_next(cur); i < numeps; ++i, cur = tu_desc_next(cur)) {
    cur = _find_desc_ep(cur, end);
    TU_ASSERT(cur < end);
    tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const*)cur;
    uint_fast32_t max_size = stm->max_payload_transfer_size;
    if (altnum && (TUSB_XFER_ISOCHRONOUS == ep->bmAttributes.xfer)) {
      /* FS must be less than or equal to max packet size */
      TU_VERIFY (tu_edpt_packet_size(ep) >= max_size);
#ifdef TUP_DCD_EDPT_ISO_ALLOC
      usbd_edpt_iso_activate(rhport, ep);
#else
      TU_ASSERT(usbd_edpt_open(rhport, ep));
#endif
    } else {
      TU_VERIFY(TUSB_XFER_BULK == ep->bmAttributes.xfer);
      TU_ASSERT(usbd_edpt_open(rhport, ep));
    }
    stm->desc.ep[i] = (uint16_t) (cur - desc);
    TU_LOG_DRV("    open EP%02x\r\n", _desc_ep_addr(cur));
  }
  if (altnum) {
    stm->state = VS_STATE_STREAMING;
  }
  TU_LOG_DRV("    done\r\n");
  return true;
}

static uint_fast16_t _prepare_in_payload(videod_streaming_interface_t *stm, uint8_t* ep_buf) {
  uint_fast16_t remaining = stm->bufsize - stm->offset;
  uint_fast16_t hdr_len   = ep_buf[0];
  uint_fast16_t pkt_len   = stm->max_payload_transfer_size;
  if (hdr_len + remaining < pkt_len) {
    pkt_len = hdr_len + remaining;
  }
  TU_ASSERT(pkt_len >= hdr_len);
  uint_fast16_t data_len = pkt_len - hdr_len;
  memcpy(&ep_buf[hdr_len], stm->buffer + stm->offset, data_len);
  stm->offset += data_len;
  remaining -= data_len;
  if (!remaining) {
    tusb_video_payload_header_t *hdr = (tusb_video_payload_header_t*) ep_buf;
    hdr->EndOfFrame = 1;
  }
  return hdr_len + data_len;
}

bool tud_video_n_streaming(uint_fast8_t ctl_idx, uint_fast8_t stm_idx)
{
  TU_ASSERT(ctl_idx < CFG_TUD_VIDEO);
  TU_ASSERT(stm_idx < CFG_TUD_VIDEO_STREAMING);
  videod_streaming_interface_t *stm = _get_instance_streaming(ctl_idx, stm_idx);
  if (!stm || !stm->desc.ep[0]) return false;
  if (stm->state == VS_STATE_PROBING) return false;

#ifdef TUP_DCD_EDPT_ISO_ALLOC
  uint8_t const *desc = _videod_itf[stm->index_vc].beg;
  uint_fast16_t ofs_ep = stm->desc.ep[0];
  tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const*)(desc + ofs_ep);
  if (ep->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS) {
    if (stm->state == VS_STATE_COMMITTED) return false;
  }
#endif

  return true;
}

bool tud_video_n_frame_xfer(uint_fast8_t ctl_idx, uint_fast8_t stm_idx, void *buffer, size_t bufsize) {
  TU_ASSERT(ctl_idx < CFG_TUD_VIDEO);
  TU_ASSERT(stm_idx < CFG_TUD_VIDEO_STREAMING);

  if (!buffer || !bufsize) return false;
  videod_streaming_interface_t *stm = _get_instance_streaming(ctl_idx, stm_idx);
  videod_streaming_epbuf_t *stm_epbuf = &_videod_streaming_epbuf[ctl_idx];

  if (!stm || !stm->desc.ep[0] || stm->buffer) return false;
  if (stm->state == VS_STATE_PROBING) return false;

  /* Find EP address */
  uint8_t const *desc = _videod_itf[stm->index_vc].beg;
  uint8_t ep_addr = 0;
  for (uint_fast8_t i = 0; i < CFG_TUD_VIDEO_STREAMING; ++i) {
    uint_fast16_t ofs_ep = stm->desc.ep[i];
    if (!ofs_ep) continue;
    ep_addr = _desc_ep_addr(desc + ofs_ep);
    break;
  }
  if (!ep_addr) return false;

  TU_VERIFY( usbd_edpt_claim(0, ep_addr) );
  /* update the packet header */
  tusb_video_payload_header_t *hdr = (tusb_video_payload_header_t*)stm_epbuf->buf;
  hdr->FrameID   ^= 1;
  hdr->EndOfFrame = 0;
  /* update the packet data */
  stm->buffer     = (uint8_t*)buffer;
  stm->bufsize    = bufsize;
  uint_fast16_t pkt_len = _prepare_in_payload(stm, stm_epbuf->buf);
  TU_ASSERT( usbd_edpt_xfer(0, ep_addr, stm_epbuf->buf, (uint16_t) pkt_len), 0);
  return true;
}

void videod_reset(uint8_t rhport) {
  (void) rhport;
  for (uint_fast8_t i = 0; i < CFG_TUD_VIDEO; ++i) {
    videod_interface_t* ctl = &_videod_itf[i];
    tu_memclr(ctl, sizeof(*ctl));
  }
  for (uint_fast8_t i = 0; i < CFG_TUD_VIDEO_STREAMING; ++i) {
    videod_streaming_interface_t *stm = &_videod_streaming_itf[i];
    tu_memclr(stm, sizeof(videod_streaming_interface_t));
  }
}

bool videod_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
  (void)result; (void)xferred_bytes;

  /* find streaming handle */
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
  videod_streaming_epbuf_t *stm_epbuf = &_videod_streaming_epbuf[itf];

  if (stm->offset < stm->bufsize) {
    /* Claim the endpoint */
    TU_VERIFY( usbd_edpt_claim(rhport, ep_addr), 0);
    uint_fast16_t pkt_len = _prepare_in_payload(stm, stm_epbuf->buf);
    TU_ASSERT( usbd_edpt_xfer(rhport, ep_addr, stm_epbuf->buf, (uint16_t) pkt_len), 0);
  } else {
    stm->buffer  = NULL;
    stm->bufsize = 0;
    stm->offset  = 0;
    tud_video_frame_xfer_complete_cb(stm->index_vc, stm->index_vs);
  }
  return true;
}

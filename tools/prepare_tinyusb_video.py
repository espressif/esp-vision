# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Prepare a scoped MIT TinyUSB video source; never edit the managed component."""
import argparse
import hashlib
from pathlib import Path

SOURCE_SHA256 = 'c3a99094c039c51de88ad06eb70807d2d645cbb552bc262577ce7b2404012315'


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError('Unsupported TinyUSB video transfer source; review the adaptation')
    return source.replace(before, after, 1)


def patch_transfer_functions(source):
    """Shared by the full-source build and the verbatim upstream function fixture."""
    source = replace_once(source, '  return hdr_len + data_len;',
                          '  stm->queued_size = (uint16_t)(hdr_len + data_len);\n'
                          '  return hdr_len + data_len;')
    source = replace_once(source, '  hdr->EndOfFrame = 0;',
                          '  hdr->EndOfFrame = 0;\n  hdr->Error = 0;\n'
                          '  stm->failed = false;\n  stm->offset = 0;')
    return replace_once(
        source, '  TU_ASSERT( usbd_edpt_xfer(0, ep_addr, stm_epbuf->buf, (uint16_t) pkt_len), 0);',
        '  if (!usbd_edpt_xfer(0, ep_addr, stm_epbuf->buf, (uint16_t) pkt_len)) {\n'
        '    esp_vision_video_release(stm, true);\n    return false;\n  }')


def patch_source(source):
    """Adapt checked upstream code; also used for verbatim host fixture functions."""
    source = replace_once(source, '  uint32_t offset;   /* offset for the next payload transfer */',
                          '  uint32_t offset;   /* offset for the next payload transfer */\n'
                          '  uint16_t queued_size; /* ESP-VISION: verify each ISO completion */\n'
                          '  bool failed; /* an ERR/EOF terminator is pending */')
    declaration = ('TU_ATTR_WEAK void tud_video_frame_xfer_failed_cb(uint_fast8_t ctl_idx, '
                   'uint_fast8_t stm_idx);\n')
    source = replace_once(source, '/** Set the alternate setting to own video streaming interface.',
                          declaration + '\n/** Set the alternate setting to own video streaming interface.')
    cancel = ('  if (stm->buffer && tud_video_frame_xfer_failed_cb) {\n'
              '    tud_video_frame_xfer_failed_cb(stm->index_vc, stm->index_vs);\n  }\n')
    source = replace_once(source, '  /* clear transfer management information */\n',
                          '  /* clear transfer management information */\n' + cancel)
    source = replace_once(source, 'void videod_reset(uint8_t rhport) {',
                          'void videod_reset(uint8_t rhport) {\n'
                          '  for (uint_fast8_t i = 0; i < CFG_TUD_VIDEO_STREAMING; ++i) {\n'
                          '    videod_streaming_interface_t *stm = &_videod_streaming_itf[i];\n' +
                          ''.join('  ' + line + '\n' for line in cancel.rstrip().splitlines()) + '  }')
    source = patch_transfer_functions(source)
    source = replace_once(source, 'bool tud_video_n_frame_xfer(',
                          '#include "esp_vision_video_transfer.inc"\n\nbool tud_video_n_frame_xfer(')
    start = source.index('bool videod_xfer_cb(')
    end = source.index('\n}\n', start) + 3
    source = source[:start] + '/* ESP-VISION completion implementation is included above. */\n' + source[end:]
    return source


def prepare(source):
    if hashlib.sha256(source.encode()).hexdigest() != SOURCE_SHA256:
        raise ValueError('Unsupported TinyUSB video_device.c; expected 0.19.0~3 source ' + SOURCE_SHA256)
    return patch_source(source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_text(prepare(args.source.read_text()))


if __name__ == '__main__':
    main()

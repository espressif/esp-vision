/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define CFG_TUD_VIDEO_STREAMING 1
#define TU_ASSERT(condition) assert(condition)

typedef enum {
    XFER_RESULT_SUCCESS,
    XFER_RESULT_FAILED,
} xfer_result_t;

typedef struct {
    uint8_t length;
    uint8_t FrameID : 1;
    uint8_t EndOfFrame : 1;
    uint8_t reserved : 4;
    uint8_t Error : 1;
    uint8_t end : 1;
} tusb_video_payload_header_t;

typedef struct {
    uint8_t index_vc;
    struct {
        uint16_t ep[1];
    } desc;
    const uint8_t *buffer;
    uint32_t bufsize;
    uint32_t offset;
    uint16_t queued_size;
    bool failed;
    bool cancel_pending;
} videod_streaming_interface_t;

typedef struct {
    const uint8_t *beg;
} videod_interface_t;

typedef struct {
    uint8_t buf[16];
} videod_streaming_epbuf_t;

static const uint8_t descriptor[] = {0, 0x85};
static videod_interface_t _videod_itf[] = {{descriptor}};
static videod_streaming_interface_t _videod_streaming_itf[] = {{.desc.ep = {1}}};
static videod_streaming_epbuf_t _videod_streaming_epbuf[1];

static const uint8_t source[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static unsigned queued, completed, cancelled;
static bool claim_ok, fail_next_queue;
static uint8_t packets[8][16];
static uint16_t lengths[8];

static uint8_t _desc_ep_addr(const uint8_t *endpoint)
{
    return *endpoint;
}

static bool usbd_edpt_claim(uint8_t rhport, uint8_t endpoint)
{
    assert(rhport == 0 && endpoint == 0x85);
    return claim_ok;
}

static bool usbd_edpt_xfer(uint8_t rhport, uint8_t endpoint, const uint8_t *data, uint16_t size)
{
    assert(rhport == 0 && endpoint == 0x85);
    assert(queued < 8 && size <= sizeof(packets[0]));
    if (fail_next_queue) {
        fail_next_queue = false;
        return false;
    }
    memcpy(packets[queued], data, size);
    lengths[queued++] = size;
    return true;
}

static uint_fast16_t _prepare_in_payload(videod_streaming_interface_t *stream, uint8_t *buffer)
{
    const uint32_t remaining = stream->bufsize - stream->offset;
    const uint32_t count = remaining < 5 ? remaining : 5;
    tusb_video_payload_header_t *header = (tusb_video_payload_header_t *)buffer;
    memcpy(buffer + sizeof(*header), stream->buffer + stream->offset, count);
    stream->offset += count;
    header->EndOfFrame = stream->offset == stream->bufsize;
    stream->queued_size = sizeof(*header) + count;
    return stream->queued_size;
}

static void esp_vision_video_retire(videod_streaming_interface_t *stream, bool failed)
{
    assert(stream->buffer == source);
    stream->buffer = NULL;
    stream->offset = 0;
    stream->failed = false;
    if (failed) {
        ++cancelled;
    } else {
        ++completed;
    }
}

#include "esp_vision_video_transfer.inc"

static void start_frame(void)
{
    videod_streaming_interface_t *stream = &_videod_streaming_itf[0];
    *stream = (videod_streaming_interface_t) {
        .desc.ep = {1}, .buffer = source, .bufsize = sizeof(source),
    };
    queued = completed = cancelled = 0;
    claim_ok = true;
    fail_next_queue = false;
    uint8_t *buffer = _videod_streaming_epbuf[0].buf;
    memset(buffer, 0, sizeof(_videod_streaming_epbuf[0].buf));
    tusb_video_payload_header_t *header = (tusb_video_payload_header_t *)buffer;
    header->length = sizeof(*header);
    header->FrameID = 1;
    usbd_edpt_xfer(0, 0x85, buffer, _prepare_in_payload(stream, buffer));
}

static void test_success(void)
{
    start_frame();
    unsigned i = 0;
    while (_videod_streaming_itf[0].buffer) {
        assert(i < queued);
        assert(videod_xfer_cb(0, 0x85, XFER_RESULT_SUCCESS, lengths[i++]));
    }
    assert(completed == 1 && cancelled == 0);
    unsigned offset = 0;
    for (i = 0; i < queued; ++i) {
        tusb_video_payload_header_t header;
        memcpy(&header, packets[i], sizeof(header));
        assert(header.FrameID == 1 && !header.Error);
        assert(header.EndOfFrame == (i == queued - 1));
        const uint16_t count = lengths[i] - sizeof(header);
        assert(!memcmp(source + offset, packets[i] + sizeof(header), count));
        offset += count;
    }
    assert(offset == sizeof(source));
}

static void test_error(xfer_result_t result, uint32_t bytes, bool fail_payload_queue)
{
    start_frame();
    fail_next_queue = fail_payload_queue;
    assert(videod_xfer_cb(0, 0x85, result, bytes));
    assert(queued == 2 && completed == 0 && cancelled == 0);
    tusb_video_payload_header_t header;
    memcpy(&header, packets[1], sizeof(header));
    assert(lengths[1] == sizeof(header) && header.FrameID == 1);
    assert(header.Error && header.EndOfFrame);
    assert(_videod_streaming_itf[0].buffer == source);
    assert(videod_xfer_cb(0, 0x85, XFER_RESULT_FAILED, 0));
    assert(queued == 2 && completed == 0 && cancelled == 1);
    assert(videod_xfer_cb(0, 0x85, XFER_RESULT_SUCCESS, sizeof(header)));
    assert(cancelled == 1);
}

int main(void)
{
    test_success();
    const uint32_t packet_size = sizeof(tusb_video_payload_header_t) + 5;
    test_error(XFER_RESULT_FAILED, 0, false);
    test_error(XFER_RESULT_SUCCESS, 0, false);
    test_error(XFER_RESULT_SUCCESS, packet_size - 1, false);
    test_error(XFER_RESULT_SUCCESS, packet_size + 1, false);
    test_error(XFER_RESULT_SUCCESS, packet_size, true);

    start_frame();
    claim_ok = false;
    assert(videod_xfer_cb(0, 0x85, XFER_RESULT_SUCCESS, packet_size));
    assert(queued == 1 && completed == 0 && cancelled == 1);

    start_frame();
    fail_next_queue = true;
    assert(videod_xfer_cb(0, 0x85, XFER_RESULT_FAILED, 0));
    assert(queued == 1 && completed == 0 && cancelled == 1);

    start_frame();
    _videod_streaming_itf[0].cancel_pending = true;
    assert(videod_xfer_cb(0, 0x85, XFER_RESULT_SUCCESS, packet_size));
    assert(queued == 1 && completed == 0 && cancelled == 0);
    assert(_videod_streaming_itf[0].offset == 5 && _videod_streaming_itf[0].buffer == source);
    test_success();
    return 0;
}

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform/ev_mux.c"
#include "../../platform/ev_vm_output.c"
#include "../../platform/ev_stdio.c"

#define MOCK_OUTPUT_CAPACITY (32768)

int64_t ev_mux_test_time_us;
bool ev_mux_test_psram_initialized = true;
bool ev_mux_test_task_create_fail;

static unsigned int s_heap_alloc_calls;
static unsigned int s_heap_free_calls;
static unsigned int s_heap_fail_next;
static unsigned int s_heap_last_caps;
static size_t s_heap_last_size;

void *heap_caps_malloc(size_t size, unsigned int caps)
{
    ++s_heap_alloc_calls;
    s_heap_last_size = size;
    s_heap_last_caps = caps;
    if (s_heap_fail_next != 0) {
        --s_heap_fail_next;
        return NULL;
    }
    return malloc(size);
}

void heap_caps_free(void *pointer)
{
    if (pointer != NULL) {
        ++s_heap_free_calls;
        free(pointer);
    }
}

void *esp_vision_memory_malloc_caps(size_t size, uint32_t caps)
{
    return heap_caps_malloc(size, caps);
}

typedef struct {
    bool current;
    uint32_t generation;
    uint8_t output[MOCK_OUTPUT_CAPACITY];
    size_t output_len;
} mock_link_t;

static mock_link_t s_mock_links[EV_TRANSPORT_TCP_LINKS];
static int s_write_calls;
static int s_would_block_calls;
static bool s_block_forever;
static size_t s_max_write;
static int s_close_calls;
static int s_lost_calls;
static bool s_mock_owner;
static ev_session_snapshot_t s_mock_session;
static int s_completion_calls;
static esp_err_t s_completion_result;
static uint32_t s_completion_context;
#define MOCK_COMPLETION_RECORDS (8)
typedef struct {
    esp_err_t result;
    uint32_t context;
} mock_completion_record_t;
static mock_completion_record_t s_completion_records[MOCK_COMPLETION_RECORDS];
static int s_switch_owner_after_delay;
static ev_link_ref_t s_switch_owner_link;
static char s_switch_owner_session[EV_SESSION_ID_HEX_LEN + 1];

static void mock_set_owner(const ev_link_ref_t *link, const char *session_id);

void ev_mux_test_delay_hook(TickType_t ticks)
{
    ev_mux_test_time_us += (int64_t)ticks * 1000;
    if (s_switch_owner_after_delay > 0) {
        --s_switch_owner_after_delay;
        if (s_switch_owner_after_delay == 0) {
            mock_set_owner(&s_switch_owner_link, s_switch_owner_session);
        }
    }
    ev_mux_tx_worker_step();
}

vprintf_like_t esp_log_set_vprintf(vprintf_like_t function)
{
    (void)function;
    return NULL;
}

void ev_control_transport_init0(void)
{
}

void ev_control_transport_start(void)
{
}

void ev_control_transport_vm_reset_begin(void)
{
}

esp_err_t ev_control_transport_send_hello(void)
{
    return ESP_OK;
}

static void mock_reset_io(void)
{
    memset(s_mock_links, 0, sizeof(s_mock_links));
    for (size_t i = 0; i < EV_TRANSPORT_TCP_LINKS; ++i) {
        s_mock_links[i].current = true;
        s_mock_links[i].generation = 1;
    }
    s_write_calls = 0;
    s_would_block_calls = 0;
    s_block_forever = false;
    s_max_write = SIZE_MAX;
    s_close_calls = 0;
    s_lost_calls = 0;
    s_mock_owner = false;
    memset(&s_mock_session, 0, sizeof(s_mock_session));
    s_completion_calls = 0;
    s_completion_result = ESP_FAIL;
    s_completion_context = 0;
    memset(s_completion_records, 0, sizeof(s_completion_records));
    s_switch_owner_after_delay = 0;
}

static void mock_reset_mux(void)
{
    assert(ev_mux_init0() == ESP_OK);
    for (size_t i = 0; i < EV_MUX_TARGET_COUNT; ++i) {
        ev_mux_target_queue_t *queue = &s_targets[i];
        queue->mutex_storage.taken = false;
        memset(queue->frames, 0, sizeof(queue->frames));
        queue->bytes_used = 0;
        queue->frames_used = 0;
        queue->congested = false;
        queue->congested_at_ms = 0;
        queue->free_blocks = (i < EV_MUX_SERIAL_TARGETS) ? UINT64_MAX : 0;
    }
    s_special_mutex_storage.taken = false;
    s_repl_producer_mutex_storage.taken = false;
    s_repl_stage_mutex_storage.taken = false;
    memset(&s_repl_stage, 0, sizeof(s_repl_stage));
    s_repl_cancelled_epoch = 0;
    s_tcp_free_blocks = UINT64_MAX;
    s_tcp_bytes_used = 0;
    s_tcp_frames_used = 0;
    s_tcp_discovery_bytes_used = 0;
    s_tcp_discovery_frames_used = 0;
    s_tcp_low_bytes_used = 0;
    s_tcp_low_frames_used = 0;
    for (size_t i = 0; i < EV_MUX_TARGET_COUNT; ++i) {
        __atomic_store_n(&s_special_partial_frame[i], (uintptr_t)0, __ATOMIC_RELEASE);
    }
    memset(s_preview_frames, 0, sizeof(s_preview_frames));
    memset(&s_large_frame, 0, sizeof(s_large_frame));
    s_preview_pending = -1;
    s_preview_active = -1;
    __atomic_store_n(&s_tx_timeout_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_drop_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_queue_full_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_stale_count, 0, __ATOMIC_RELAXED);
    ev_vm_output_reset();
    mock_reset_io();
}

static void assert_mux_init_unpublished(void)
{
    assert(!s_initialized);
    assert(!s_init_mutex_storage.taken);
    assert(s_arena_slab == NULL);
    assert(s_tcp_arena == NULL);
    assert(s_tx_task == NULL);
    for (size_t i = 0; i < EV_MUX_TARGET_COUNT; ++i) {
        assert(s_targets[i].arena == NULL);
    }
}

static void test_arena_init_failure_retry_and_idempotence(void)
{
    unsigned int allocations_before = s_heap_alloc_calls;
    unsigned int frees_before = s_heap_free_calls;

#if CONFIG_SPIRAM
    ev_mux_test_psram_initialized = false;
    assert(ev_mux_init0() == ESP_ERR_NO_MEM);
    assert(s_heap_alloc_calls == allocations_before);
    assert_mux_init_unpublished();
    ev_mux_test_psram_initialized = true;
#endif

    s_heap_fail_next = 1;
    assert(ev_mux_init0() == ESP_ERR_NO_MEM);
    assert(s_heap_alloc_calls == (allocations_before + 1));
    assert(s_heap_free_calls == frees_before);
    assert(s_heap_last_size == (64u * 1024u));
#if CONFIG_SPIRAM
    assert(s_heap_last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    assert(s_heap_last_caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#endif
    assert_mux_init_unpublished();

    ev_mux_test_task_create_fail = true;
    assert(ev_mux_init0() == ESP_ERR_NO_MEM);
    assert(s_heap_alloc_calls == (allocations_before + 2));
    assert(s_heap_free_calls == (frees_before + 1));
    assert_mux_init_unpublished();
    ev_mux_test_task_create_fail = false;

    assert(ev_mux_init0() == ESP_OK);
    assert(s_heap_alloc_calls == (allocations_before + 3));
    assert(s_heap_free_calls == (frees_before + 1));
    assert(s_initialized);
    assert(s_arena_slab != NULL);
    assert(s_tcp_arena == (s_arena_slab + (EV_MUX_SERIAL_TARGETS * EV_MUX_QUEUE_BYTES)));
    for (size_t i = 0; i < EV_MUX_SERIAL_TARGETS; ++i) {
        assert(s_targets[i].arena == (s_arena_slab + (i * EV_MUX_QUEUE_BYTES)));
    }
    for (size_t i = EV_MUX_SERIAL_TARGETS; i < EV_MUX_TARGET_COUNT; ++i) {
        assert(s_targets[i].arena == s_tcp_arena);
    }

    assert(ev_mux_init0() == ESP_OK);
    assert(s_heap_alloc_calls == (allocations_before + 3));
    assert(s_heap_free_calls == (frees_before + 1));
}

static ev_link_ref_t mock_tcp_link(uint8_t slot)
{
    ev_link_ref_t link = {
        .transport_id = EV_TRANSPORT_TCP,
        .slot = slot,
        .generation = s_mock_links[slot].generation,
    };
    return link;
}

bool ev_transport_link_is_current(const ev_link_ref_t *link)
{
    if ((link == NULL) || (link->transport_id != EV_TRANSPORT_TCP) ||
            (link->slot >= EV_TRANSPORT_TCP_LINKS)) {
        return false;
    }
    const mock_link_t *state = &s_mock_links[link->slot];
    return state->current && (state->generation == link->generation);
}

bool ev_transport_link_ref(ev_transport_id_t id, uint8_t slot, ev_link_ref_t *out_link)
{
    if ((out_link == NULL) || (id != EV_TRANSPORT_TCP) ||
            (slot >= EV_TRANSPORT_TCP_LINKS) || !s_mock_links[slot].current) {
        return false;
    }
    *out_link = mock_tcp_link(slot);
    return true;
}

bool ev_transport_id_from_name(const char *name, ev_transport_id_t *out_id)
{
    if ((name == NULL) || (out_id == NULL) || (strcmp(name, "tcp") != 0)) {
        return false;
    }
    *out_id = EV_TRANSPORT_TCP;
    return true;
}

ev_io_result_t ev_transport_write(const ev_link_ref_t *link, const void *buffer, size_t length)
{
    if (!ev_transport_link_is_current(link)) {
        return (ev_io_result_t) {
            EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE
        };
    }
    ++s_write_calls;
    if (s_block_forever || (s_would_block_calls > 0)) {
        if (s_would_block_calls > 0) {
            --s_would_block_calls;
        }
        return (ev_io_result_t) {
            EV_IO_WOULD_BLOCK, 0, ESP_OK
        };
    }
    size_t transferred = (length < s_max_write) ? length : s_max_write;
    mock_link_t *state = &s_mock_links[link->slot];
    assert((state->output_len + transferred) <= sizeof(state->output));
    memcpy(state->output + state->output_len, buffer, transferred);
    state->output_len += transferred;
    return (ev_io_result_t) {
        EV_IO_PROGRESS, transferred, ESP_OK
    };
}

void ev_transport_link_close(const ev_link_ref_t *link)
{
    if (ev_transport_link_is_current(link)) {
        mock_link_t *state = &s_mock_links[link->slot];
        state->current = false;
        ++state->generation;
        ++s_close_calls;
    }
}

bool ev_session_snapshot(ev_session_snapshot_t *out_snapshot)
{
    if ((out_snapshot == NULL) || !s_mock_owner) {
        return false;
    }
    *out_snapshot = s_mock_session;
    return true;
}

uint32_t ev_session_input_generation(void)
{
    return s_mock_session.input_generation;
}

bool ev_session_output_ready(const ev_link_ref_t *link, const char *session_id)
{
    return s_mock_owner && s_mock_session.has_owner &&
           (s_mock_session.state == EV_SESSION_OWNED) && s_mock_session.handshake_complete &&
           (link != NULL) && (session_id != NULL) &&
           (link->transport_id == s_mock_session.owner_link.transport_id) &&
           (link->slot == s_mock_session.owner_link.slot) &&
           (link->generation == s_mock_session.owner_link.generation) &&
           (strcmp(session_id, s_mock_session.session_id) == 0);
}

bool ev_session_link_lost(const ev_link_ref_t *link, ev_session_change_reason_t reason)
{
    (void)reason;
    if (s_mock_owner && (link != NULL) &&
            (link->slot == s_mock_session.owner_link.slot) &&
            (link->generation == s_mock_session.owner_link.generation)) {
        s_mock_owner = false;
        s_mock_session.has_owner = false;
        s_mock_session.state = EV_SESSION_FREE;
        ++s_lost_calls;
        return true;
    }
    return false;
}

const char *ev_channel_get(ev_stream_t stream)
{
    (void)stream;
    return "tcp";
}

typedef struct {
    ev_control_json_metadata_t metadata;
    const uint8_t *payload;
    size_t payload_len;
    size_t total_len;
} mock_parsed_frame_t;

static bool mock_parse_frame(const uint8_t *wire,
                             size_t wire_len,
                             size_t offset,
                             mock_parsed_frame_t *out)
{
    if ((wire == NULL) || (out == NULL) || (offset >= wire_len)) {
        return false;
    }
    size_t line_end = offset;
    while ((line_end + 1 < wire_len) &&
            !((wire[line_end] == '\r') && (wire[line_end + 1] == '\n'))) {
        ++line_end;
    }
    if (line_end + 1 >= wire_len) {
        return false;
    }
    char header[EV_MUX_HEADER_MAX];
    size_t header_len = line_end + 2 - offset;
    if (header_len >= sizeof(header)) {
        return false;
    }
    memcpy(header, wire + offset, header_len);
    header[header_len] = '\0';
    unsigned int metadata_len;
    unsigned int payload_len;
    unsigned int crc;
    if (sscanf(header, "\x1e" "EVMUX/1 h=%u p=%u c=%x\r\n",
               &metadata_len, &payload_len, &crc) != 3) {
        return false;
    }
    size_t total_len = header_len + (size_t)metadata_len + (size_t)payload_len + 1u;
    if ((total_len > wire_len - offset) || (wire[offset + total_len - 1] != 0x1f)) {
        return false;
    }
    char metadata[EV_MUX_METADATA_MAX + 1];
    if (metadata_len > EV_MUX_METADATA_MAX) {
        return false;
    }
    memcpy(metadata, wire + offset + header_len, metadata_len);
    metadata[metadata_len] = '\0';
    if (!ev_control_json_parse_metadata(metadata, metadata_len, &out->metadata)) {
        return false;
    }
    out->payload = wire + offset + header_len + metadata_len;
    out->payload_len = payload_len;
    out->total_len = total_len;
    (void)crc;
    return true;
}

static size_t mock_drain_worker(void)
{
    for (size_t i = 0; i < 512; ++i) {
        bool queued = false;
        for (size_t target = 0; target < EV_MUX_TARGET_COUNT; ++target) {
            queued |= s_targets[target].frames_used != 0;
        }
        queued |= s_preview_pending >= 0 || s_preview_active >= 0;
        if (!queued) {
            break;
        }
        ev_mux_tx_worker_step();
    }
    return s_mock_links[0].output_len;
}

static size_t mock_make_frame(const char *metadata,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint8_t *out,
                              size_t out_capacity)
{
    char header[EV_MUX_HEADER_MAX];
    uint32_t crc = ev_mux_crc32(payload, payload_len);
    int header_len = snprintf(header,
                              sizeof(header),
                              "\x1e"
                              "EVMUX/1 h=%zu p=%zu c=%08" PRIX32 "\r\n",
                              strlen(metadata),
                              payload_len,
                              crc);
    assert((header_len > 0) && ((size_t)header_len < sizeof(header)));
    size_t metadata_len = strlen(metadata);
    size_t total = (size_t)header_len + metadata_len + payload_len + 1;
    assert(total <= out_capacity);
    memcpy(out, header, (size_t)header_len);
    memcpy(out + header_len, metadata, metadata_len);
    memcpy(out + header_len + metadata_len, payload, payload_len);
    out[total - 1] = 0x1f;
    return total;
}

static void mock_completion(esp_err_t result,
                            ev_link_ref_t link,
                            const void *context,
                            size_t context_len)
{
    (void)link;
    assert(context_len == sizeof(s_completion_context));
    memcpy(&s_completion_context, context, context_len);
    s_completion_result = result;
    if ((size_t)s_completion_calls < MOCK_COMPLETION_RECORDS) {
        s_completion_records[s_completion_calls].result = result;
        s_completion_records[s_completion_calls].context = s_completion_context;
    }
    ++s_completion_calls;
}

static void mock_release_completion(esp_err_t result,
                                    ev_link_ref_t link,
                                    const void *context,
                                    size_t context_len)
{
    mock_completion(result, link, context, context_len);
    if ((result == ESP_OK) && s_mock_owner && s_mock_session.has_owner &&
            (s_mock_session.state == EV_SESSION_RELEASING) &&
            (link.transport_id == s_mock_session.owner_link.transport_id) &&
            (link.slot == s_mock_session.owner_link.slot) &&
            (link.generation == s_mock_session.owner_link.generation)) {
        s_mock_owner = false;
        s_mock_session.has_owner = false;
        s_mock_session.state = EV_SESSION_FREE;
    }
}

static void mock_begin_release(void)
{
    assert(s_mock_owner && s_mock_session.has_owner);
    assert(s_mock_session.state == EV_SESSION_OWNED);
    s_mock_session.state = EV_SESSION_RELEASING;
    ++s_mock_session.input_generation;
    assert(!ev_session_output_ready(&s_mock_session.owner_link, s_mock_session.session_id));
}

static void test_short_writes_and_owned_payload(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    uint8_t first_payload[] = { 'a', 'b', 'c', 'd', 'e' };
    uint8_t second_payload[] = { '1', '2', '3', '4' };
    const char *first_metadata = "{\"n\":1}";
    const char *second_metadata = "{\"n\":2}";
    ev_mux_submit_options_t options = {
        .link = &link,
        .tx_class = EV_MUX_TX_CONTROL,
    };
    assert(ev_mux_submit_link(&options,
                              first_metadata,
                              first_payload,
                              sizeof(first_payload),
                              NULL) == ESP_OK);
    assert(ev_mux_submit_link(&options,
                              second_metadata,
                              second_payload,
                              sizeof(second_payload),
                              NULL) == ESP_OK);
    memset(first_payload, 0, sizeof(first_payload));
    memset(second_payload, 0, sizeof(second_payload));

    s_would_block_calls = 1;
    s_max_write = 3;
    for (size_t i = 0; (i < 256) && (s_targets[3].frames_used != 0); ++i) {
        ev_mux_process_target(3);
    }
    assert(s_targets[3].frames_used == 0);

    uint8_t expected[256];
    size_t expected_len = mock_make_frame(first_metadata,
                                          (const uint8_t *)"abcde",
                                          5,
                                          expected,
                                          sizeof(expected));
    expected_len += mock_make_frame(second_metadata,
                                    (const uint8_t *)"1234",
                                    4,
                                    expected + expected_len,
                                    sizeof(expected) - expected_len);
    assert(s_mock_links[0].output_len == expected_len);
    assert(memcmp(s_mock_links[0].output, expected, expected_len) == 0);
    assert(s_write_calls > 10);
}

static void test_tcp_links_share_queue_and_reservations(void)
{
    mock_reset_mux();
    ev_link_ref_t links[2] = { mock_tcp_link(0), mock_tcp_link(1) };
    ev_mux_submit_options_t options = {
        .link = &links[0],
        .tx_class = EV_MUX_TX_CONTROL,
    };
    uint8_t byte = 'x';
    for (size_t i = 0; i < 6; ++i) {
        options.link = &links[i % 2];
        assert(ev_mux_submit_link(&options, "{}", &byte, 1, NULL) == ESP_OK);
    }
    options.link = &links[1];
    assert(ev_mux_submit_link(&options, "{}", &byte, 1, NULL) == ESP_ERR_TIMEOUT);

    options.link = &links[0];
    options.session_id = "0123456789abcdef0123456789abcdef";
    assert(ev_mux_submit_link(&options, "{}", &byte, 1, NULL) == ESP_OK);
    assert(ev_mux_submit_link(&options, "{}", &byte, 1, NULL) == ESP_OK);
    assert(s_tcp_frames_used == EV_MUX_QUEUE_FRAMES);
    assert(s_tcp_bytes_used <= EV_MUX_QUEUE_BYTES);
    assert(ev_mux_submit_link(&options, "{}", &byte, 1, NULL) == ESP_ERR_TIMEOUT);

    mock_reset_mux();
    links[0] = mock_tcp_link(0);
    options.link = &links[0];
    options.session_id = "0123456789abcdef0123456789abcdef";
    options.tx_class = EV_MUX_TX_REPL;
    for (size_t i = 0; i < 6; ++i) {
        assert(ev_mux_submit_link(&options, "{}", &byte, 1, NULL) == ESP_OK);
    }
    assert(ev_mux_submit_link(&options, "{}", &byte, 1, NULL) == ESP_ERR_TIMEOUT);
    options.tx_class = EV_MUX_TX_CONTROL;
    assert(ev_mux_submit_link(&options, "{}", &byte, 1, NULL) == ESP_OK);
    assert(ev_mux_submit_link(&options, "{}", &byte, 1, NULL) == ESP_OK);
    assert(s_tcp_frames_used == EV_MUX_QUEUE_FRAMES);
}

static void test_full_frame_deadline_and_callback_copy(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    s_mock_owner = true;
    s_mock_session.state = EV_SESSION_OWNED;
    s_mock_session.has_owner = true;
    s_mock_session.handshake_complete = true;
    s_mock_session.owner_link = link;
    memcpy(s_mock_session.session_id, session_id, sizeof(session_id));
    s_mock_session.vm_epoch = 17;

    uint32_t callback_context = UINT32_C(0xfeedbeef);
    ev_mux_submit_options_t options = {
        .link = &link,
        .session_id = session_id,
        .vm_epoch = 17,
        .tx_class = EV_MUX_TX_CONTROL,
        .completion = mock_completion,
        .completion_context = &callback_context,
        .completion_context_len = sizeof(callback_context),
    };
    ev_mux_test_time_us = 100000;
    assert(ev_mux_submit_link(&options, "{}", "payload", 7, NULL) == ESP_OK);
    callback_context = 0;

    s_max_write = 1;
    ev_mux_process_target(3);
    assert(s_mock_links[0].output_len == 1);
    ev_mux_test_time_us += INT64_C(1000000);
    ev_mux_process_target(3);
    assert(s_mock_links[0].output_len == 2);
    ev_mux_test_time_us += INT64_C(501000);
    ev_mux_process_target(3);

    assert(s_completion_calls == 1);
    assert(s_completion_result == ESP_ERR_TIMEOUT);
    assert(s_completion_context == UINT32_C(0xfeedbeef));
    assert(ev_mux_tx_timeout_count() == 1);
    assert(s_lost_calls == 1);
    assert(s_close_calls == 1);
    assert(!ev_transport_link_is_current(&link));
}

static void mock_set_owner(const ev_link_ref_t *link, const char *session_id)
{
    s_mock_owner = true;
    s_mock_session.state = EV_SESSION_OWNED;
    s_mock_session.has_owner = true;
    s_mock_session.owner_link = *link;
    memcpy(s_mock_session.session_id, session_id, strlen(session_id) + 1);
    s_mock_session.vm_epoch = 23;
    s_mock_session.input_generation = 73;
    s_mock_session.transition_id = 5;
    s_mock_session.handshake_complete = true;
    s_mock_session.vm_ready = true;
}

static ev_vm_output_origin_t mock_output_origin(const ev_link_ref_t *link,
                                                const char *session_id)
{
    ev_vm_output_origin_t origin = {
        .link = *link,
        .vm_epoch = s_mock_session.vm_epoch,
        .input_generation = s_mock_session.input_generation,
        .transition_id = s_mock_session.transition_id,
    };
    memcpy(origin.session_id, session_id, strlen(session_id) + 1);
    return origin;
}

static void test_partial_repl_finishes_before_release_ack(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_vm_output_origin_t origin = mock_output_origin(&link, session_id);
    const char repl_metadata[] =
        "{\"sid\":\"user\",\"seq\":1,\"channel\":\"repl.stdout\","
        "\"type\":\"data\",\"encoding\":\"utf-8\","
        "\"sessionId\":\"0123456789abcdef0123456789abcdef\",\"vmEpoch\":23,\"ts_ms\":1}";
    const char ack_metadata[] =
        "{\"sid\":\"user\",\"seq\":2,\"channel\":\"user.rpc\","
        "\"type\":\"rsp\",\"method\":\"session.release\",\"encoding\":\"json\","
        "\"sessionId\":\"0123456789abcdef0123456789abcdef\",\"vmEpoch\":23,\"ts_ms\":2}";
    static const char repl_payload[] = "old output\n";
    static const char ack_payload[] = "{\"ok\":true,\"accepted\":true}";
    uint32_t repl_context = UINT32_C(0x11111111);
    uint32_t ack_context = UINT32_C(0x22222222);
    ev_mux_submit_options_t repl_options = {
        .link = &link,
        .session_id = session_id,
        .vm_epoch = 23,
        .tx_class = EV_MUX_TX_REPL,
        .completion = mock_completion,
        .completion_context = &repl_context,
        .completion_context_len = sizeof(repl_context),
        .vm_output_origin = &origin,
    };
    s_max_write = 4;
    assert(ev_mux_submit_link(&repl_options,
                              repl_metadata,
                              repl_payload,
                              sizeof(repl_payload) - 1,
                              NULL) == ESP_OK);
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    ev_mux_frame_t *partial = &s_targets[EV_MUX_SERIAL_TARGETS].frames[0];
    assert(partial->used && (partial->tx_offset > 0) &&
           (partial->tx_offset < partial->total_len));
    size_t partial_wire_len = s_mock_links[0].output_len;

    mock_begin_release();
    assert(!ev_session_output_ready(&link, session_id));
    ev_mux_submit_options_t ack_options = {
        .link = &link,
        .session_id = session_id,
        .tx_class = EV_MUX_TX_SESSION,
        .completion = mock_release_completion,
        .completion_context = &ack_context,
        .completion_context_len = sizeof(ack_context),
    };
    assert(ev_mux_submit_link(&ack_options,
                              ack_metadata,
                              ack_payload,
                              sizeof(ack_payload) - 1,
                              NULL) == ESP_OK);
    assert(s_targets[EV_MUX_SERIAL_TARGETS].frames_used == 2);

    for (size_t i = 0; (i < 256) &&
            (s_targets[EV_MUX_SERIAL_TARGETS].frames_used != 0); ++i) {
        ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    }
    assert(s_targets[EV_MUX_SERIAL_TARGETS].frames_used == 0);
    assert(s_completion_calls == 2);
    assert(s_completion_records[0].result == ESP_OK);
    assert(s_completion_records[0].context == repl_context);
    assert(s_completion_records[1].result == ESP_OK);
    assert(s_completion_records[1].context == ack_context);
    assert(s_mock_session.state == EV_SESSION_FREE);
    assert(!s_mock_owner && !s_mock_session.has_owner);
    assert(ev_mux_tx_timeout_count() == 0);

    uint8_t expected[1024];
    size_t expected_len = mock_make_frame(repl_metadata,
                                          (const uint8_t *)repl_payload,
                                          sizeof(repl_payload) - 1,
                                          expected,
                                          sizeof(expected));
    expected_len += mock_make_frame(ack_metadata,
                                    (const uint8_t *)ack_payload,
                                    sizeof(ack_payload) - 1,
                                    expected + expected_len,
                                    sizeof(expected) - expected_len);
    assert(partial_wire_len < expected_len);
    assert(s_mock_links[0].output_len == expected_len);
    assert(memcmp(s_mock_links[0].output, expected, expected_len) == 0);
}

static void test_partial_preview_finishes_before_release_ack(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    const char preview_metadata[] =
        "{\"sid\":\"user\",\"seq\":11,\"channel\":\"preview.frame\","
        "\"type\":\"data\",\"encoding\":\"binary\",\"ts_ms\":11}";
    const char bound_preview_metadata[] =
        "{\"sid\":\"user\",\"seq\":11,\"channel\":\"preview.frame\","
        "\"type\":\"data\",\"encoding\":\"binary\",\"ts_ms\":11,"
        "\"sessionId\":\"0123456789abcdef0123456789abcdef\",\"vmEpoch\":23}";
    const char ack_metadata[] =
        "{\"sid\":\"user\",\"seq\":12,\"channel\":\"user.rpc\","
        "\"type\":\"rsp\",\"method\":\"session.release\",\"encoding\":\"json\","
        "\"sessionId\":\"0123456789abcdef0123456789abcdef\",\"vmEpoch\":23,\"ts_ms\":12}";
    static const uint8_t preview_payload[] = { 0xff, 0xd8, 0x31, 0x1f, 0xff };
    static const char ack_payload[] = "{\"ok\":true,\"accepted\":true}";
    uint32_t ack_context = UINT32_C(0x55555555);
    s_max_write = 4;
    assert(ev_mux_write_lossy(EV_STREAM_USER,
                              preview_metadata,
                              preview_payload,
                              sizeof(preview_payload)) == ESP_OK);
    assert(s_preview_pending >= 0);
    int8_t preview_pool = s_preview_pending;
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    ev_mux_frame_t *partial = &s_preview_frames[preview_pool].frame;
    assert(s_preview_frames[preview_pool].used && (partial->tx_offset > 0) &&
           (partial->tx_offset < partial->total_len));
    assert(s_preview_active == preview_pool && (s_preview_pending < 0));

    mock_begin_release();
    ev_mux_submit_options_t ack_options = {
        .link = &link,
        .session_id = session_id,
        .tx_class = EV_MUX_TX_SESSION,
        .completion = mock_release_completion,
        .completion_context = &ack_context,
        .completion_context_len = sizeof(ack_context),
    };
    assert(ev_mux_submit_link(&ack_options,
                              ack_metadata,
                              ack_payload,
                              sizeof(ack_payload) - 1,
                              NULL) == ESP_OK);

    for (size_t i = 0; (i < 256) &&
            ((s_targets[EV_MUX_SERIAL_TARGETS].frames_used != 0) ||
             (s_preview_active >= 0)); ++i) {
        ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    }
    assert(s_targets[EV_MUX_SERIAL_TARGETS].frames_used == 0);
    assert(s_preview_active < 0 && s_preview_pending < 0);
    assert(s_completion_calls == 1);
    assert(s_completion_result == ESP_OK);
    assert(s_completion_context == ack_context);
    assert(s_mock_session.state == EV_SESSION_FREE);

    uint8_t expected[1024];
    size_t expected_len = mock_make_frame(bound_preview_metadata,
                                          preview_payload,
                                          sizeof(preview_payload),
                                          expected,
                                          sizeof(expected));
    expected_len += mock_make_frame(ack_metadata,
                                    (const uint8_t *)ack_payload,
                                    sizeof(ack_payload) - 1,
                                    expected + expected_len,
                                    sizeof(expected) - expected_len);
    assert(s_mock_links[0].output_len == expected_len);
    assert(memcmp(s_mock_links[0].output, expected, expected_len) == 0);
}

static void test_partial_repl_deadline_cancels_release_ack(void)
{
    mock_reset_mux();
    ev_mux_test_time_us = INT64_C(100000);
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_vm_output_origin_t origin = mock_output_origin(&link, session_id);
    const char repl_metadata[] =
        "{\"sid\":\"user\",\"seq\":3,\"channel\":\"repl.stdout\","
        "\"type\":\"data\",\"encoding\":\"utf-8\","
        "\"sessionId\":\"0123456789abcdef0123456789abcdef\",\"vmEpoch\":23,\"ts_ms\":3}";
    const char ack_metadata[] =
        "{\"sid\":\"user\",\"seq\":4,\"channel\":\"user.rpc\","
        "\"type\":\"rsp\",\"method\":\"session.release\",\"encoding\":\"json\","
        "\"sessionId\":\"0123456789abcdef0123456789abcdef\",\"vmEpoch\":23,\"ts_ms\":4}";
    static const char repl_payload[] = "blocked old output\n";
    static const char ack_payload[] = "{\"ok\":true,\"accepted\":true}";
    uint32_t repl_context = UINT32_C(0x33333333);
    uint32_t ack_context = UINT32_C(0x44444444);
    ev_mux_submit_options_t repl_options = {
        .link = &link,
        .session_id = session_id,
        .vm_epoch = 23,
        .tx_class = EV_MUX_TX_REPL,
        .completion = mock_completion,
        .completion_context = &repl_context,
        .completion_context_len = sizeof(repl_context),
        .vm_output_origin = &origin,
    };
    s_max_write = 4;
    assert(ev_mux_submit_link(&repl_options,
                              repl_metadata,
                              repl_payload,
                              sizeof(repl_payload) - 1,
                              NULL) == ESP_OK);
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    ev_mux_frame_t *partial = &s_targets[EV_MUX_SERIAL_TARGETS].frames[0];
    assert(partial->used && partial->attempted && (partial->tx_offset != 0));
    uint32_t frame_timeout_ms = partial->frame_timeout_ms;
    size_t partial_wire_len = s_mock_links[0].output_len;

    mock_begin_release();
    ev_mux_submit_options_t ack_options = {
        .link = &link,
        .session_id = session_id,
        .tx_class = EV_MUX_TX_SESSION,
        .completion = mock_release_completion,
        .completion_context = &ack_context,
        .completion_context_len = sizeof(ack_context),
    };
    assert(ev_mux_submit_link(&ack_options,
                              ack_metadata,
                              ack_payload,
                              sizeof(ack_payload) - 1,
                              NULL) == ESP_OK);
    assert(s_targets[EV_MUX_SERIAL_TARGETS].frames_used == 2);

    s_block_forever = true;
    ev_mux_test_time_us += ((int64_t)frame_timeout_ms + 1) * 1000;
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    assert(s_completion_calls == 2);
    assert(s_completion_records[0].result == ESP_ERR_TIMEOUT);
    assert(s_completion_records[0].context == repl_context);
    assert(s_completion_records[1].result == ESP_ERR_INVALID_STATE);
    assert(s_completion_records[1].context == ack_context);
    assert(s_targets[EV_MUX_SERIAL_TARGETS].frames_used == 0);
    assert(ev_mux_tx_timeout_count() == 1);
    assert(s_lost_calls == 1);
    assert(!s_mock_owner && !s_mock_session.has_owner);
    assert(s_mock_session.state == EV_SESSION_FREE);
    assert(s_mock_links[0].output_len == partial_wire_len);
    assert(!ev_transport_link_is_current(&link));
}

static void test_partial_old_generation_is_not_sent_to_new_owner(void)
{
    mock_reset_mux();
    ev_link_ref_t old_link = mock_tcp_link(0);
    static const char old_session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&old_link, old_session_id);
    ev_mux_submit_options_t old_options = {
        .link = &old_link,
        .session_id = old_session_id,
        .vm_epoch = 23,
        .tx_class = EV_MUX_TX_REPL,
    };
    s_max_write = 4;
    assert(ev_mux_submit_link(&old_options, "{}", "old-session-data", 16, NULL) == ESP_OK);
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    assert(s_mock_links[0].output_len == 4);

    // Reusing this TCP slot creates a new generation and a new socket. The
    // old socket's partial prefix is no longer observable on the new handle.
    ++s_mock_links[0].generation;
    s_mock_links[0].output_len = 0;
    ev_link_ref_t new_link = mock_tcp_link(0);
    static const char new_session_id[] = "fedcba9876543210fedcba9876543210";
    mock_set_owner(&new_link, new_session_id);
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    assert(s_targets[EV_MUX_SERIAL_TARGETS].frames_used == 0);
    assert(s_mock_links[0].output_len == 0);
    assert(ev_mux_tx_stale_count() == 1);
    assert(s_lost_calls == 0 && s_close_calls == 0);

    const char new_metadata[] = "{\"sid\":\"user\",\"seq\":5,\"channel\":\"repl.stdout\","
                                "\"type\":\"data\",\"encoding\":\"utf-8\","
                                "\"sessionId\":\"fedcba9876543210fedcba9876543210\","
                                "\"vmEpoch\":23,\"ts_ms\":5}";
    static const char new_payload[] = "new-owner-data\n";
    ev_mux_submit_options_t new_options = {
        .link = &new_link,
        .session_id = new_session_id,
        .vm_epoch = 23,
        .tx_class = EV_MUX_TX_REPL,
    };
    assert(ev_mux_submit_link(&new_options,
                              new_metadata,
                              new_payload,
                              sizeof(new_payload) - 1,
                              NULL) == ESP_OK);
    for (size_t i = 0; (i < 128) &&
            (s_targets[EV_MUX_SERIAL_TARGETS].frames_used != 0); ++i) {
        ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    }
    uint8_t expected[512];
    size_t expected_len = mock_make_frame(new_metadata,
                                          (const uint8_t *)new_payload,
                                          sizeof(new_payload) - 1,
                                          expected,
                                          sizeof(expected));
    assert(s_mock_links[0].output_len == expected_len);
    assert(memcmp(s_mock_links[0].output, expected, expected_len) == 0);
    assert(s_targets[EV_MUX_SERIAL_TARGETS].frames_used == 0);
}

static void test_lazy_pools_latest_preview_and_large_copy(void)
{
    mock_reset_mux();
    assert(!ev_mux_preview_slot_available());
    assert(!ev_mux_large_payload_available());
    assert(s_preview_buffers[0] == NULL);
    assert(s_preview_buffers[1] == NULL);
    assert(s_large_buffer == NULL);

    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_mux_submit_options_t preview_options = {
        .link = &link,
        .session_id = session_id,
        .vm_epoch = 23,
        .tx_class = EV_MUX_TX_PREVIEW,
    };
    assert(ev_mux_preview_available(EV_STREAM_USER));
    assert((s_preview_buffers[0] != NULL) || (s_preview_buffers[1] != NULL));
    assert(ev_mux_submit_preview_link(&preview_options, "{\"p\":1}", "old", 3) == ESP_OK);
    assert(ev_mux_submit_preview_link(&preview_options, "{\"p\":2}", "latest", 6) == ESP_OK);
    assert(ev_mux_tx_drop_count() == 1);

    for (size_t i = 0; (i < 64) && (s_preview_pending >= 0 || s_preview_active >= 0); ++i) {
        ev_mux_process_target(3);
    }
    uint8_t expected_preview[128];
    size_t expected_preview_len = mock_make_frame("{\"p\":2}",
                                                  (const uint8_t *)"latest",
                                                  6,
                                                  expected_preview,
                                                  sizeof(expected_preview));
    assert(s_mock_links[0].output_len == expected_preview_len);
    assert(memcmp(s_mock_links[0].output, expected_preview, expected_preview_len) == 0);

    mock_reset_mux();
    link = mock_tcp_link(0);
    mock_set_owner(&link, session_id);
    ev_mux_submit_options_t large_options = {
        .link = &link,
        .session_id = session_id,
        .vm_epoch = 23,
        .tx_class = EV_MUX_TX_CONTROL,
    };
    assert(ev_mux_large_payload_available());
    assert(s_large_buffer != NULL);

    static uint8_t large_payload[20000];
    static uint8_t expected_large[21000];
    for (size_t i = 0; i < sizeof(large_payload); ++i) {
        large_payload[i] = (uint8_t)(i % 251);
    }
    size_t expected_large_len = mock_make_frame("{\"large\":true}",
                                                large_payload,
                                                sizeof(large_payload),
                                                expected_large,
                                                sizeof(expected_large));
    assert(expected_large_len > EV_MUX_QUEUE_BYTES);
    assert(ev_mux_submit_large_link(&large_options,
                                    "{\"large\":true}",
                                    large_payload,
                                    sizeof(large_payload),
                                    NULL) == ESP_OK);
    memset(large_payload, 0, sizeof(large_payload));
    for (size_t i = 0; (i < 256) && s_large_frame.used; ++i) {
        ev_mux_process_target(3);
    }
    assert(!s_large_frame.used);
    assert(s_mock_links[0].output_len == expected_large_len);
    assert(memcmp(s_mock_links[0].output, expected_large, expected_large_len) == 0);
}

static void test_partial_preview_lock_miss_preserves_frame_boundary(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    ev_link_ref_t other_link = mock_tcp_link(1);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_mux_submit_options_t preview_options = {
        .link = &link,
        .session_id = session_id,
        .vm_epoch = 23,
        .tx_class = EV_MUX_TX_PREVIEW,
    };
    const uint8_t preview_payload[] = { 0xff, 0xd8, 1, 2, 3, 0xff, 0xd9 };
    const char preview_metadata[] = "{\"preview\":1}";
    const char control_metadata[] = "{\"control\":1}";
    const char other_metadata[] = "{\"other\":1}";
    s_max_write = 4;
    assert(ev_mux_submit_preview_link(&preview_options,
                                      preview_metadata,
                                      preview_payload,
                                      sizeof(preview_payload)) == ESP_OK);
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    assert(s_preview_active >= 0);
    ev_mux_special_frame_t *active = &s_preview_frames[s_preview_active];
    assert(active->frame.tx_offset == 4);
    assert(__atomic_load_n(&s_special_partial_frame[EV_MUX_SERIAL_TARGETS],
                           __ATOMIC_ACQUIRE) == (uintptr_t)active);

    ev_mux_submit_options_t pending_options = preview_options;
    pending_options.vm_epoch = 24;
    assert(ev_mux_submit_preview_link(&pending_options, "{\"preview\":2}", "old", 3) == ESP_OK);
    int8_t pending_pool = s_preview_pending;
    assert(pending_pool >= 0 && pending_pool != s_preview_active);
    assert(ev_mux_submit_preview_link(&pending_options, "{\"preview\":3}", "new", 3) == ESP_OK);
    assert(s_preview_pending == pending_pool);
    assert(__atomic_load_n(&s_special_partial_frame[EV_MUX_SERIAL_TARGETS],
                           __ATOMIC_ACQUIRE) == (uintptr_t)active);

    // Cancelling the unstarted pending preview must not clear the marker for
    // the separate active preview on this same target.
    ev_mux_cancel_vm_epoch(24);
    assert(s_preview_pending < 0);
    assert(s_preview_active >= 0 && &s_preview_frames[s_preview_active] == active);
    assert(__atomic_load_n(&s_special_partial_frame[EV_MUX_SERIAL_TARGETS],
                           __ATOMIC_ACQUIRE) == (uintptr_t)active);

    ev_mux_submit_options_t control_options = preview_options;
    control_options.tx_class = EV_MUX_TX_CONTROL;
    assert(ev_mux_submit_link(&control_options, control_metadata, "c", 1, NULL) == ESP_OK);
    ev_mux_submit_options_t other_options = {
        .link = &other_link,
        .tx_class = EV_MUX_TX_CONTROL,
    };
    assert(ev_mux_submit_link(&other_options, other_metadata, "o", 1, NULL) == ESP_OK);

    size_t output_before = s_mock_links[0].output_len;
    s_special_mutex_storage.taken = true;
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    assert(s_mock_links[0].output_len == output_before);
    assert(s_targets[EV_MUX_SERIAL_TARGETS].frames_used == 1);
    assert(__atomic_load_n(&s_special_partial_frame[EV_MUX_SERIAL_TARGETS],
                           __ATOMIC_ACQUIRE) == (uintptr_t)active);

    // A held special mutex blocks only the target with a partial special frame.
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS + other_link.slot);
    assert(s_mock_links[1].output_len != 0);
    s_special_mutex_storage.taken = false;

    for (size_t i = 0; (i < 128) &&
            ((s_targets[EV_MUX_SERIAL_TARGETS].frames_used != 0) ||
             (s_preview_active >= 0)); ++i) {
        ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    }
    assert(s_preview_active < 0 && s_preview_pending < 0);
    assert(__atomic_load_n(&s_special_partial_frame[EV_MUX_SERIAL_TARGETS],
                           __ATOMIC_ACQUIRE) == 0);

    uint8_t expected[1024];
    size_t expected_len = mock_make_frame(preview_metadata,
                                          preview_payload,
                                          sizeof(preview_payload),
                                          expected,
                                          sizeof(expected));
    expected_len += mock_make_frame(control_metadata,
                                    (const uint8_t *)"c",
                                    1,
                                    expected + expected_len,
                                    sizeof(expected) - expected_len);
    assert(s_mock_links[0].output_len == expected_len);
    assert(memcmp(s_mock_links[0].output, expected, expected_len) == 0);
}

static void test_partial_large_frame_lock_miss_preserves_frame_boundary(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    ev_link_ref_t other_link = mock_tcp_link(1);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_mux_submit_options_t large_options = {
        .link = &link,
        .session_id = session_id,
        .vm_epoch = 23,
        .tx_class = EV_MUX_TX_CONTROL,
    };
    static uint8_t large_payload[700];
    static uint8_t expected[1024];
    for (size_t i = 0; i < sizeof(large_payload); ++i) {
        large_payload[i] = (uint8_t)(i % 251);
    }
    const char large_metadata[] = "{\"large\":true}";
    const char control_metadata[] = "{\"control\":true}";
    s_max_write = 4;
    assert(ev_mux_submit_large_link(&large_options,
                                    large_metadata,
                                    large_payload,
                                    sizeof(large_payload),
                                    NULL) == ESP_OK);
    for (size_t i = 0; (i < 16) && (s_large_frame.frame.tx_offset == 0); ++i) {
        ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    }
    assert(s_large_frame.used && s_large_frame.frame.tx_offset == 4);
    assert(__atomic_load_n(&s_special_partial_frame[EV_MUX_SERIAL_TARGETS],
                           __ATOMIC_ACQUIRE) == (uintptr_t)&s_large_frame);

    ev_mux_submit_options_t control_options = large_options;
    control_options.tx_class = EV_MUX_TX_CONTROL;
    assert(ev_mux_submit_link(&control_options, control_metadata, "c", 1, NULL) == ESP_OK);
    size_t output_before = s_mock_links[0].output_len;
    s_special_mutex_storage.taken = true;
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    assert(s_mock_links[0].output_len == output_before);
    assert(s_targets[EV_MUX_SERIAL_TARGETS].frames_used == 1);
    assert(__atomic_load_n(&s_special_partial_frame[EV_MUX_SERIAL_TARGETS],
                           __ATOMIC_ACQUIRE) == (uintptr_t)&s_large_frame);

    ev_mux_submit_options_t other_options = {
        .link = &other_link,
        .tx_class = EV_MUX_TX_CONTROL,
    };
    assert(ev_mux_submit_link(&other_options, "{\"other\":true}", "o", 1, NULL) == ESP_OK);
    ev_mux_process_target(EV_MUX_SERIAL_TARGETS + other_link.slot);
    assert(s_mock_links[1].output_len != 0);

    s_special_mutex_storage.taken = false;
    for (size_t i = 0; (i < 512) &&
            (s_targets[EV_MUX_SERIAL_TARGETS].frames_used != 0 || s_large_frame.used); ++i) {
        ev_mux_process_target(EV_MUX_SERIAL_TARGETS);
    }
    assert(!s_large_frame.used && s_targets[EV_MUX_SERIAL_TARGETS].frames_used == 0);
    assert(__atomic_load_n(&s_special_partial_frame[EV_MUX_SERIAL_TARGETS],
                           __ATOMIC_ACQUIRE) == 0);

    size_t expected_len = mock_make_frame(large_metadata,
                                          large_payload,
                                          sizeof(large_payload),
                                          expected,
                                          sizeof(expected));
    expected_len += mock_make_frame(control_metadata,
                                    (const uint8_t *)"c",
                                    1,
                                    expected + expected_len,
                                    sizeof(expected) - expected_len);
    assert(s_mock_links[0].output_len == expected_len);
    assert(memcmp(s_mock_links[0].output, expected, expected_len) == 0);
}

static void test_late_old_epoch_reply_does_not_release_current_owner(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_mux_cancel_vm_epoch(22);
    ev_mux_submit_options_t old = {
        .link = &link,
        .session_id = session_id,
        .vm_epoch = 22,
        .tx_class = EV_MUX_TX_CONTROL,
    };
    assert(ev_mux_submit_link(&old, "{}", "old reply", 9, NULL) == ESP_OK);
    ev_mux_process_target(3);
    assert(s_mock_links[0].output_len == 0);
    assert(ev_mux_tx_stale_count() == 1);
    assert(s_mock_owner && s_lost_calls == 0 && s_close_calls == 0);
    old.vm_epoch = 0;
    assert(ev_mux_submit_link(&old, "{}", "keepalive", 9, NULL) == ESP_OK);
    for (size_t i = 0; i < 16; ++i) {
        ev_mux_process_target(3);
    }
    assert(s_mock_links[0].output_len > 0);
    assert(s_mock_owner && s_lost_calls == 0);
}

static void assert_session_metadata(const ev_control_json_metadata_t *metadata,
                                    const char *session_id,
                                    uint32_t vm_epoch)
{
    assert(metadata->has_session_id);
    assert(metadata->has_vm_epoch);
    assert(strcmp(metadata->session_id, session_id) == 0);
    assert(metadata->vm_epoch == vm_epoch);
}

static void test_stdio_character_echo_coalesces_and_binds_wire_metadata(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_stdio_mux_enabled_flag = true;

    uint8_t line[512];
    memset(line, 'x', sizeof(line) - 1);
    for (size_t i = 0; i < sizeof(line) - 1; ++i) {
        char byte = (char)line[i];
        assert(ev_stdio_write_repl_stdout(&byte, 1));
    }
    assert(s_repl_stage.used);
    assert(s_repl_stage.payload_len == sizeof(line) - 1);
    assert(s_targets[3].frames_used == 0);

    line[sizeof(line) - 1] = '\n';
    assert(ev_stdio_write_repl_stdout((const char *)&line[sizeof(line) - 1], 1));
    assert(!s_repl_stage.used);
    assert(s_targets[3].frames_used == 1);
    mock_drain_worker();

    mock_parsed_frame_t frame;
    assert(mock_parse_frame(s_mock_links[0].output, s_mock_links[0].output_len, 0, &frame));
    assert(frame.payload_len == sizeof(line));
    assert(memcmp(frame.payload, line, sizeof(line)) == 0);
    assert(strcmp(frame.metadata.channel, "repl.stdout") == 0);
    assert_session_metadata(&frame.metadata, session_id, 23);
    assert(frame.total_len == s_mock_links[0].output_len);
}

static void test_stdio_prompt_flushes_after_coalesce_window_and_preserves_order(void)
{
    mock_reset_mux();
    ev_mux_test_time_us = INT64_C(10000000);
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_stdio_mux_enabled_flag = true;

    assert(ev_stdio_write_repl_stdout(">>> ", 4));
    assert(s_repl_stage.used && (s_repl_stage.payload_len == 4));
    ev_mux_test_time_us += 4000;
    ev_mux_tx_worker_step();
    assert(s_mock_links[0].output_len == 0);
    assert(s_targets[3].frames_used == 0);

    ev_mux_test_time_us += 1000;
    ev_mux_tx_worker_step();
    assert(s_targets[3].frames_used == 1);
    assert(ev_stdio_write_repl_stdout("next\n", 5));
    assert(s_targets[3].frames_used == 2);
    mock_drain_worker();

    mock_parsed_frame_t first;
    mock_parsed_frame_t second;
    assert(mock_parse_frame(s_mock_links[0].output, s_mock_links[0].output_len, 0, &first));
    assert(first.payload_len == 4);
    assert(memcmp(first.payload, ">>> ", 4) == 0);
    assert_session_metadata(&first.metadata, session_id, 23);
    assert(mock_parse_frame(s_mock_links[0].output,
                            s_mock_links[0].output_len,
                            first.total_len,
                            &second));
    assert(second.payload_len == 5);
    assert(memcmp(second.payload, "next\n", 5) == 0);
    assert_session_metadata(&second.metadata, session_id, 23);
    assert(first.total_len + second.total_len == s_mock_links[0].output_len);
}

static void test_full_repl_chunk_precedes_idle_prompt(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_stdio_mux_enabled_flag = true;

    uint8_t full_chunk[EV_MUX_REPL_STAGE_BYTES];
    memset(full_chunk, 'e', sizeof(full_chunk));
    assert(ev_stdio_write_repl_stdout((const char *)full_chunk, sizeof(full_chunk)));
    assert(!s_repl_stage.used);
    assert(s_targets[3].frames_used == 1);
    assert(ev_stdio_write_repl_stdout(">>> ", 4));
    assert(s_repl_stage.used && (s_repl_stage.payload_len == 4));
    ev_mux_test_time_us += EV_MUX_REPL_COALESCE_MS * 1000;
    mock_drain_worker();

    mock_parsed_frame_t first;
    mock_parsed_frame_t prompt;
    assert(mock_parse_frame(s_mock_links[0].output, s_mock_links[0].output_len, 0, &first));
    assert(first.payload_len == sizeof(full_chunk));
    assert(memcmp(first.payload, full_chunk, sizeof(full_chunk)) == 0);
    assert_session_metadata(&first.metadata, session_id, 23);
    assert(mock_parse_frame(s_mock_links[0].output,
                            s_mock_links[0].output_len,
                            first.total_len,
                            &prompt));
    assert(prompt.payload_len == 4);
    assert(memcmp(prompt.payload, ">>> ", 4) == 0);
    assert_session_metadata(&prompt.metadata, session_id, 23);
    assert(first.total_len + prompt.total_len == s_mock_links[0].output_len);
}

static void test_repl_burst_larger_than_six_frames_retries_with_worker_progress(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_stdio_mux_enabled_flag = true;

    uint8_t payload[4096];
    uint8_t reconstructed[sizeof(payload)];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)('a' + (i % 26));
    }
    assert(ev_stdio_write_repl_stdout((const char *)payload, sizeof(payload)));
    mock_drain_worker();

    size_t offset = 0;
    size_t wire_offset = 0;
    size_t frame_count = 0;
    while (wire_offset < s_mock_links[0].output_len) {
        mock_parsed_frame_t frame;
        assert(mock_parse_frame(s_mock_links[0].output,
                                s_mock_links[0].output_len,
                                wire_offset,
                                &frame));
        assert(frame.payload_len == EV_MUX_REPL_STAGE_BYTES);
        assert_session_metadata(&frame.metadata, session_id, 23);
        assert((offset + frame.payload_len) <= sizeof(reconstructed));
        memcpy(reconstructed + offset, frame.payload, frame.payload_len);
        offset += frame.payload_len;
        wire_offset += frame.total_len;
        ++frame_count;
    }
    assert(frame_count == 8);
    assert(offset == sizeof(payload));
    assert(memcmp(reconstructed, payload, sizeof(payload)) == 0);
}

static void test_repl_channel_changes_flush_in_order(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_stdio_mux_enabled_flag = true;

    assert(ev_stdio_write_repl_stdout("A", 1));
    assert(ev_stdio_write_repl_stderr("B", 1));
    assert(ev_stdio_write_repl_stdout("C", 1));
    assert(s_targets[3].frames_used == 2);
    ev_mux_test_time_us += 5000;
    mock_drain_worker();

    size_t wire_offset = 0;
    const char expected_channels[3][16] = {
        "repl.stdout",
        "repl.stderr",
        "repl.stdout",
    };
    const char expected_payloads[3] = { 'A', 'B', 'C' };
    for (size_t i = 0; i < 3; ++i) {
        mock_parsed_frame_t frame;
        assert(mock_parse_frame(s_mock_links[0].output,
                                s_mock_links[0].output_len,
                                wire_offset,
                                &frame));
        assert(strcmp(frame.metadata.channel, expected_channels[i]) == 0);
        assert_session_metadata(&frame.metadata, session_id, 23);
        assert(frame.payload_len == 1);
        assert(frame.payload[0] == (uint8_t)expected_payloads[i]);
        wire_offset += frame.total_len;
    }
    assert(wire_offset == s_mock_links[0].output_len);
}

static void test_repl_queue_admission_wait_is_bounded(void)
{
    mock_reset_mux();
    ev_mux_test_time_us = INT64_C(20000000);
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_stdio_mux_enabled_flag = true;

    uint8_t payload[EV_MUX_REPL_STAGE_BYTES];
    memset(payload, 'b', sizeof(payload));
    payload[sizeof(payload) - 1] = '\n';
    assert(xSemaphoreTake(s_targets[3].mutex, 0) == pdTRUE);
    int64_t started_us = ev_mux_test_time_us;
    assert(!ev_stdio_write_repl_stdout((const char *)payload, sizeof(payload)));
    int64_t elapsed_us = ev_mux_test_time_us - started_us;
    xSemaphoreGive(s_targets[3].mutex);

    assert(elapsed_us >= INT64_C(2000000));
    assert(elapsed_us <= INT64_C(2001000));
    assert(ev_mux_tx_queue_full_count() == 1);
    assert(ev_mux_tx_drop_count() == 1);
    assert(s_lost_calls == 1);
    assert(s_mock_links[0].output_len == 0);
}

static void test_repl_blocked_link_expires_without_blocking_worker(void)
{
    mock_reset_mux();
    ev_mux_test_time_us = INT64_C(30000000);
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_stdio_mux_enabled_flag = true;
    s_block_forever = true;

    assert(ev_stdio_write_repl_stdout(">", 1));
    int64_t started_us = ev_mux_test_time_us;
    ev_mux_test_delay_hook(5);
    for (size_t i = 0; (i < 2000) && (s_targets[3].frames_used != 0); ++i) {
        ev_mux_test_delay_hook(1);
    }
    int64_t elapsed_us = ev_mux_test_time_us - started_us;
    assert(s_targets[3].frames_used == 0);
    assert(elapsed_us >= INT64_C(1500000));
    assert(elapsed_us <= INT64_C(1506000));
    assert(ev_mux_tx_timeout_count() == 1);
    assert(ev_mux_tx_drop_count() == 1);
    assert(s_lost_calls == 1);
    assert(s_mock_links[0].output_len == 0);
}

static void test_repl_handoff_and_epoch_cancel_never_retarget_stage(void)
{
    mock_reset_mux();
    ev_link_ref_t first_link = mock_tcp_link(0);
    static const char first_session[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&first_link, first_session);
    ev_stdio_mux_enabled_flag = true;
    assert(ev_stdio_write_repl_stdout("old", 3));
    assert(s_repl_stage.used);

    ev_link_ref_t second_link = mock_tcp_link(1);
    static const char second_session[] = "fedcba9876543210fedcba9876543210";
    mock_set_owner(&second_link, second_session);
    ev_mux_test_time_us += 5000;
    ev_mux_tx_worker_step();
    assert(s_mock_links[0].output_len == 0);
    assert(s_mock_links[1].output_len == 0);
    assert(!s_repl_stage.used);
    assert(ev_mux_tx_stale_count() == 1);

    mock_reset_mux();
    first_link = mock_tcp_link(0);
    mock_set_owner(&first_link, first_session);
    ev_stdio_mux_enabled_flag = true;
    assert(ev_stdio_write_repl_stdout("reset-tail", 10));
    ev_mux_cancel_vm_epoch(23);
    ev_mux_test_time_us += 5000;
    ev_mux_tx_worker_step();
    assert(s_mock_links[0].output_len == 0);
    assert(!s_repl_stage.used);
    assert(ev_mux_tx_stale_count() == 1);
}

static void test_repl_multiframe_write_drops_old_snapshot_during_handoff(void)
{
    mock_reset_mux();
    ev_link_ref_t first_link = mock_tcp_link(0);
    static const char first_session[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&first_link, first_session);
    ev_stdio_mux_enabled_flag = true;
    ev_mux_submit_options_t queued = {
        .link = &first_link,
        .session_id = first_session,
        .vm_epoch = 23,
        .tx_class = EV_MUX_TX_REPL,
    };
    uint8_t byte = 'q';
    for (size_t i = 0; i < 6; ++i) {
        assert(ev_mux_submit_link(&queued, "{}", &byte, 1, NULL) == ESP_OK);
    }

    s_switch_owner_link = mock_tcp_link(1);
    memcpy(s_switch_owner_session,
           "fedcba9876543210fedcba9876543210",
           sizeof(s_switch_owner_session));
    s_switch_owner_after_delay = 1;
    uint8_t payload[1024];
    memset(payload, 'r', sizeof(payload));
    payload[sizeof(payload) - 1] = '\n';
    assert(!ev_stdio_write_repl_stdout((const char *)payload, sizeof(payload)));
    assert(s_mock_session.owner_link.slot == 1);
    assert(!s_repl_stage.used);
    for (size_t i = 0; i < 16 && s_targets[3].frames_used != 0; ++i) {
        ev_mux_tx_worker_step();
    }
    assert(s_mock_links[0].output_len == 0);
    assert(s_mock_links[1].output_len == 0);
    assert(ev_mux_tx_stale_count() > 0);
}

static void test_stream_metadata_binding_rejects_conflicts_and_overflow(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);
    ev_stdio_mux_enabled_flag = true;

    const char bound[] =
        "{\"channel\":\"repl.stdout\",\"type\":\"data\",\"seq\":7,"
        "\"sessionId\":\"0123456789abcdef0123456789abcdef\",\"vmEpoch\":23}";
    assert(ev_mux_write_class(EV_STREAM_USER,
                              EV_MUX_TX_REPL,
                              bound,
                              "ok\n",
                              3) == ESP_OK);
    mock_drain_worker();
    mock_parsed_frame_t frame;
    assert(mock_parse_frame(s_mock_links[0].output, s_mock_links[0].output_len, 0, &frame));
    assert_session_metadata(&frame.metadata, session_id, 23);
    assert(frame.payload_len == 3);

    mock_reset_mux();
    link = mock_tcp_link(0);
    mock_set_owner(&link, session_id);
    const char conflict[] =
        "{\"channel\":\"repl.stdout\",\"type\":\"data\",\"seq\":8,"
        "\"sessionId\":\"fedcba9876543210fedcba9876543210\",\"vmEpoch\":23}";
    assert(ev_mux_write_class(EV_STREAM_USER,
                              EV_MUX_TX_REPL,
                              conflict,
                              "x\n",
                              2) == ESP_ERR_INVALID_STATE);
    assert(!s_repl_stage.used && (s_targets[3].frames_used == 0));

    char oversized[EV_MUX_METADATA_MAX];
    size_t prefix_len = (size_t)snprintf(oversized,
                                         sizeof(oversized),
                                         "{\"channel\":\"repl.stdout\",\"type\":\"data\","
                                         "\"seq\":9,\"pad\":\"");
    memset(oversized + prefix_len, 'z', 300);
    size_t oversized_len = prefix_len + 300;
    oversized[oversized_len++] = '"';
    oversized[oversized_len++] = '}';
    oversized[oversized_len] = '\0';
    assert(oversized_len < sizeof(oversized));
    assert(ev_mux_write_class(EV_STREAM_USER,
                              EV_MUX_TX_REPL,
                              oversized,
                              "x\n",
                              2) == ESP_ERR_INVALID_SIZE);
    assert(!s_repl_stage.used && (s_targets[3].frames_used == 0));
}

static void test_preview_wire_metadata_uses_same_owner_snapshot(void)
{
    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    mock_set_owner(&link, session_id);

    const char metadata[] =
        "{\"sid\":\"preview\",\"channel\":\"preview.frame\",\"type\":\"data\","
        "\"seq\":1,\"encoding\":\"binary\"}";
    assert(ev_mux_write_lossy(EV_STREAM_USER, metadata, "jpeg", 4) == ESP_OK);
    mock_drain_worker();
    mock_parsed_frame_t frame;
    assert(mock_parse_frame(s_mock_links[0].output, s_mock_links[0].output_len, 0, &frame));
    assert(strcmp(frame.metadata.channel, "preview.frame") == 0);
    assert_session_metadata(&frame.metadata, session_id, 23);
    assert(frame.payload_len == 4);
    assert(memcmp(frame.payload, "jpeg", 4) == 0);
}

static void test_vm_output_origin_never_retargets_after_handoff(void)
{
    mock_reset_mux();
    ev_link_ref_t old_link = mock_tcp_link(0);
    ev_link_ref_t new_link = mock_tcp_link(1);
    static const char old_session[] = "0123456789abcdef0123456789abcdef";
    static const char new_session[] = "fedcba9876543210fedcba9876543210";
    mock_set_owner(&old_link, old_session);
    ev_vm_output_origin_t old_origin = mock_output_origin(&old_link, old_session);
    ev_vm_output_scope_t scope;
    assert(ev_vm_output_scope_push(&old_origin, &scope));
    ev_stdio_mux_enabled_flag = true;

    assert(ev_stdio_write_repl_stdout("queued old output\n", 18));
    const char preview_metadata[] =
        "{\"sid\":\"preview\",\"channel\":\"preview.frame\",\"type\":\"data\","
        "\"seq\":1,\"encoding\":\"binary\"}";
    assert(ev_mux_write_lossy(EV_STREAM_USER, preview_metadata, "jpeg", 4) == ESP_OK);
    assert(s_targets[3].frames_used == 1);
    assert(s_preview_pending >= 0);

    mock_set_owner(&new_link, new_session);
    s_mock_session.input_generation = 74;
    s_mock_session.transition_id = 6;
    for (size_t i = 0; i < 8; ++i) {
        ev_mux_tx_worker_step();
    }
    assert(s_mock_links[0].output_len == 0);
    assert(s_mock_links[1].output_len == 0);
    assert(s_targets[3].frames_used == 0);
    assert(s_preview_pending < 0);
    assert(ev_mux_tx_stale_count() >= 2);

    // The still-active old execution scope must keep targeting its captured
    // origin and fail closed after handoff, including stdout, stderr, preview.
    assert(!ev_stdio_write_repl_stdout("late stdout\n", 12));
    assert(!ev_stdio_write_repl_stderr("late stderr\n", 12));
    assert(ev_mux_write_lossy(EV_STREAM_USER, preview_metadata, "late", 4) ==
           ESP_ERR_INVALID_STATE);
    assert(!s_repl_stage.used);
    assert(s_targets[3].frames_used == 0);
    assert(s_targets[4].frames_used == 0);
    assert(s_preview_pending < 0);
    assert(s_mock_links[0].output_len == 0);
    assert(s_mock_links[1].output_len == 0);
    assert(ev_mux_tx_stale_count() >= 5);

    ev_vm_output_scope_pop(&scope);
    assert(ev_stdio_write_repl_stdout("new owner\n", 10));
    for (size_t i = 0; i < 8 && s_targets[4].frames_used != 0; ++i) {
        ev_mux_tx_worker_step();
    }
    mock_parsed_frame_t frame;
    assert(mock_parse_frame(s_mock_links[1].output,
                            s_mock_links[1].output_len,
                            0,
                            &frame));
    assert_session_metadata(&frame.metadata, new_session, 23);
    assert(frame.payload_len == 10);
    assert(memcmp(frame.payload, "new owner\n", 10) == 0);
    assert(s_mock_links[0].output_len == 0);
}

static void test_vm_output_origin_rejects_generation_and_transition_changes(void)
{
    static const char session_id[] = "0123456789abcdef0123456789abcdef";

    mock_reset_mux();
    ev_link_ref_t link = mock_tcp_link(0);
    mock_set_owner(&link, session_id);
    ev_vm_output_origin_t origin = mock_output_origin(&link, session_id);
    ev_vm_output_scope_t scope;
    assert(ev_vm_output_scope_push(&origin, &scope));
    ev_stdio_mux_enabled_flag = true;
    ++s_mock_session.input_generation;
    assert(!ev_stdio_write_repl_stdout("stale generation\n", 17));
    assert(s_targets[3].frames_used == 0);
    assert(!s_repl_stage.used);
    assert(ev_mux_tx_stale_count() == 1);
    ev_vm_output_scope_pop(&scope);

    mock_reset_mux();
    link = mock_tcp_link(0);
    mock_set_owner(&link, session_id);
    origin = mock_output_origin(&link, session_id);
    assert(ev_vm_output_scope_push(&origin, &scope));
    ev_stdio_mux_enabled_flag = true;
    ++s_mock_session.transition_id;
    assert(!ev_stdio_write_repl_stderr("stale transition\n", 17));
    assert(s_targets[3].frames_used == 0);
    assert(!s_repl_stage.used);
    assert(ev_mux_tx_stale_count() == 1);
    ev_vm_output_scope_pop(&scope);
}

static void test_queued_vm_output_origin_is_rechecked_by_worker(void)
{
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    const char preview_metadata[] =
        "{\"sid\":\"preview\",\"channel\":\"preview.frame\",\"type\":\"data\","
        "\"seq\":1,\"encoding\":\"binary\"}";

    for (size_t change = 0; change < 2; ++change) {
        mock_reset_mux();
        ev_link_ref_t link = mock_tcp_link(0);
        mock_set_owner(&link, session_id);
        ev_vm_output_origin_t origin = mock_output_origin(&link, session_id);
        ev_vm_output_scope_t scope;
        assert(ev_vm_output_scope_push(&origin, &scope));
        ev_stdio_mux_enabled_flag = true;

        assert(ev_stdio_write_repl_stdout("queued\n", 7));
        assert(ev_mux_write_lossy(EV_STREAM_USER, preview_metadata, "jpeg", 4) == ESP_OK);
        assert(s_targets[3].frames_used == 1);
        assert(s_preview_pending >= 0);

        if (change == 0) {
            ++s_mock_session.input_generation;
        } else {
            ++s_mock_session.transition_id;
        }
        for (size_t i = 0; i < 8; ++i) {
            ev_mux_tx_worker_step();
        }
        assert(s_mock_links[0].output_len == 0);
        assert(s_targets[3].frames_used == 0);
        assert(s_preview_pending < 0);
        assert(ev_mux_tx_stale_count() >= 2);
        ev_vm_output_scope_pop(&scope);
    }
}

static void test_unowned_repl_metadata_stays_unbound(void)
{
    mock_reset_mux();
    ev_stdio_mux_enabled_flag = true;
    assert(ev_stdio_write_repl_stdout("free\n", 5));
    mock_drain_worker();
    mock_parsed_frame_t frame;
    assert(mock_parse_frame(s_mock_links[0].output, s_mock_links[0].output_len, 0, &frame));
    assert(!frame.metadata.has_session_id);
    assert(!frame.metadata.has_vm_epoch);
    assert(frame.payload_len == 5);
    assert(memcmp(frame.payload, "free\n", 5) == 0);

    const char old_binding[] =
        "{\"channel\":\"repl.stdout\",\"type\":\"data\",\"seq\":4,"
        "\"sessionId\":\"0123456789abcdef0123456789abcdef\",\"vmEpoch\":23}";
    assert(ev_mux_write_class(EV_STREAM_USER,
                              EV_MUX_TX_REPL,
                              old_binding,
                              "x\n",
                              2) == ESP_ERR_INVALID_STATE);
}

int main(void)
{
    ev_mux_test_time_us = 0;
    test_arena_init_failure_retry_and_idempotence();
    test_short_writes_and_owned_payload();
    test_tcp_links_share_queue_and_reservations();
    test_full_frame_deadline_and_callback_copy();
    test_partial_repl_finishes_before_release_ack();
    test_partial_repl_deadline_cancels_release_ack();
    test_partial_old_generation_is_not_sent_to_new_owner();
    test_lazy_pools_latest_preview_and_large_copy();
    test_partial_preview_lock_miss_preserves_frame_boundary();
    test_partial_large_frame_lock_miss_preserves_frame_boundary();
    test_partial_preview_finishes_before_release_ack();
    test_late_old_epoch_reply_does_not_release_current_owner();
    test_stdio_character_echo_coalesces_and_binds_wire_metadata();
    test_stdio_prompt_flushes_after_coalesce_window_and_preserves_order();
    test_full_repl_chunk_precedes_idle_prompt();
    test_repl_burst_larger_than_six_frames_retries_with_worker_progress();
    test_repl_channel_changes_flush_in_order();
    test_repl_queue_admission_wait_is_bounded();
    test_repl_blocked_link_expires_without_blocking_worker();
    test_repl_handoff_and_epoch_cancel_never_retarget_stage();
    test_repl_multiframe_write_drops_old_snapshot_during_handoff();
    test_stream_metadata_binding_rejects_conflicts_and_overflow();
    test_preview_wire_metadata_uses_same_owner_snapshot();
    test_vm_output_origin_never_retargets_after_handoff();
    test_vm_output_origin_rejects_generation_and_transition_changes();
    test_queued_vm_output_origin_is_rechecked_by_worker();
    test_unowned_repl_metadata_stays_unbound();
    puts("EV-MUX TX fault-injection tests passed");
    return 0;
}

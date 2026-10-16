/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ev_control_transport.h"
#include "ev_launcher_state.h"
#include "ev_file_transfer.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extmod/vfs.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "py/mphal.h"
#include "py/nlr.h"
#include "py/ringbuf.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "shared/readline/readline.h"

#include "boardconfig.h"
#include "camera.h"
#include "display.h"
#include "ev_channel.h"
#include "ev_session.h"
#include "ev_control_json.h"
#include "ev_mux.h"
#include "ev_tcp.h"
#include "ev_repl_queue.h"
#include "ev_vm_output.h"
#include "ev_stdio.h"
#include "network_service.h"
#include "firmware_info.h"
#include "device_identity.h"
#include "jpeg.h"
#include "sdcard.h"
#include "storage.h"
#include "storage_writer.h"
#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif
#ifndef NO_QSTR
#include "imlib.h"
#endif

#if MICROPY_HW_ESP_USB_SERIAL_JTAG
#include "usb_serial_jtag.h"
#endif

#if MICROPY_HW_USB_CDC
#include "shared/tinyusb/mp_usbd_cdc.h"
// Implemented in the overlay shared/tinyusb/mp_usbd_cdc.c.
extern void mp_usbd_cdc_transport_pump(void);
extern void mp_usbd_cdc_transport_rx_state(bool filter_generation,
                                           bool *mounted,
                                           uint32_t *generation,
                                           bool *generation_valid);
extern size_t mp_usbd_cdc_transport_rx_read(uint32_t expected_generation,
                                            void *buffer,
                                            size_t capacity);
extern bool mp_usbd_cdc_transport_rx_restart(uint32_t *generation_out);
extern void mp_usbd_cdc_transport_clear_rx_if_unmounted(void);
#endif

#ifndef ESP_VISION_BOARD_TYPE
#define ESP_VISION_BOARD_TYPE "unknown"
#endif

#ifndef ESP_VISION_BOARD_ARCH
#define ESP_VISION_BOARD_ARCH "unknown"
#endif

#define EV_CONTROL_METADATA_MAX (384)
#define EV_CONTROL_PAYLOAD_MAX  (2048)
#define EV_CONTROL_HEADER_MAX   (72)
#define EV_CONTROL_REPL_RX_MAX  (EV_REPL_QUEUE_CAPACITY)
#define EV_CONTROL_FILE_READ_MAX (384)
#define EV_CONTROL_VM_QUEUE_LEN (2)
#define EV_CONTROL_REPL_INPUT_MAX (EV_CONTROL_REPL_RX_MAX - 1)
// Control dispatch nests JSON parsing, network request hashing and IDF crypto
// calls. Both IDF 5.5 and 6.1 overflow an 8 KiB stack on network.configure.
#define EV_CONTROL_TRANSPORT_TASK_STACK_SIZE (16 * 1024)
#define EV_CONTROL_TRANSPORT_TASK_PRIORITY (ESP_TASK_PRIO_MIN + 2)
#define EV_CONTROL_TRANSPORT_PUMP_MS (2)
#define EV_CONTROL_RX_CHUNK_MAX (256)
#define EV_CONTROL_FRAME_IDLE_TIMEOUT_MS (3000U)
#define EV_CONTROL_FRAME_TOTAL_TIMEOUT_MS (10000U)
#define EV_CONTROL_RESPONSE_CONTEXTS (6)
#ifndef MICROPY_HW_UART_REPL_BAUD
#define MICROPY_HW_UART_REPL_BAUD (115200)
#endif

// script.write stages data in <path>.tmp before publishing it with rename().
// Keep the public path contract within the MicroPython VFS path capacity after
// adding that suffix. MICROPY_ALLOC_PATH_MAX is a buffer capacity, so one byte
// is reserved for the terminating NUL by the sizeof(".tmp") subtraction.
#define EV_CONTROL_PATH_TMP_SUFFIX ".tmp"
#define EV_CONTROL_PATH_MAX (MICROPY_ALLOC_PATH_MAX - sizeof(EV_CONTROL_PATH_TMP_SUFFIX))
// Keep enough room to classify an over-limit path as PATH_TOO_LONG instead of
// letting the JSON extractor report a generic missing/invalid argument.
#define EV_CONTROL_PATH_PARSE_CAP (EV_CONTROL_METADATA_MAX + 1)
#define EV_CONTROL_PATH_TMP_CAP (MICROPY_ALLOC_PATH_MAX)

_Static_assert(EV_CONTROL_PATH_MAX > 0, "EV control path capacity must be positive");

typedef enum {
    EV_CONTROL_PARSE_SOF = 0,
    EV_CONTROL_PARSE_HEADER,
    EV_CONTROL_PARSE_METADATA,
    EV_CONTROL_PARSE_PAYLOAD,
    EV_CONTROL_PARSE_EOF,
} ev_control_parse_state_t;

typedef struct {
    ev_control_parse_state_t state;
    bool link_bound;
    ev_link_ref_t link;
    uint32_t frame_start_ms;
    uint32_t last_progress_ms;
    char header[EV_CONTROL_HEADER_MAX];
    size_t header_len;
    char metadata[EV_CONTROL_METADATA_MAX + 1];
    size_t metadata_len;
    size_t metadata_pos;
    uint8_t payload[EV_CONTROL_PAYLOAD_MAX + 1];
    uint8_t *file_payload;
    size_t payload_len;
    size_t payload_pos;
    uint32_t expected_crc;
} ev_control_parser_t;

typedef enum {
    EV_RPC_USER = 0,
    EV_RPC_DEBUG,
} ev_control_rpc_domain_t;

// Complete RPC frames that must execute in VM context (they touch MicroPython
// objects/VFS/camera). Produced by the transport task and consumed by the VM
// task; queue indices and slots are protected by s_vm_queue_lock.
typedef struct {
    uint16_t payload_len;
    uint8_t domain;
    ev_link_ref_t link;
    char session_id[EV_SESSION_ID_HEX_LEN + 1];
    uint32_t vm_epoch;
    uint32_t input_generation;
    uint32_t transition_id;
    uint32_t queued_ms;
    uint32_t seq;
    char method[EV_CONTROL_JSON_METHOD_MAX + 1];
    uint8_t payload[EV_CONTROL_PAYLOAD_MAX + 1];
} ev_control_vm_frame_t;

// A queued frame the VM never dispatched (user code never returns to the
// REPL) must not clog the tiny queue forever: entries older than this are
// expired with a VM_TIMEOUT error, checked at push time — which is exactly
// when the host retries.
#define EV_CONTROL_VM_EXPIRE_MS (5000)

// The frame parsers are owned by the transport task (single consumer of the
// physical ingress ring buffers once EV-MUX is enabled).
static ev_control_parser_t s_parsers[EV_TRANSPORT_MAX][EV_TRANSPORT_LINK_SLOTS_MAX];
static ev_repl_queue_t s_repl_rx;
// Producers: transport task (repl.stdin) and VM task (script.run injection).
// Consumer: VM task. Producers serialize on s_repl_rx_mux.
static portMUX_TYPE s_repl_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_control_seq;
static portMUX_TYPE s_seq_mux = portMUX_INITIALIZER_UNLOCKED;
// Each task that dispatches a frame owns a separate response context. The
// critical section only assigns/finds a slot; it is never held through an RPC
// or VM operation, so TCP/serial discovery cannot retarget a VM response.
typedef struct {
    TaskHandle_t task;
    ev_control_rpc_domain_t rpc_domain;
    ev_link_ref_t link;
    bool has_link;
    char session_id[EV_SESSION_ID_HEX_LEN + 1];
    uint32_t vm_epoch;
    uint32_t tx_vm_epoch;
    uint32_t input_generation;
    uint32_t transition_id;
    ev_mux_completion_cb_t completion;
    uint8_t completion_context[EV_MUX_TX_CONTEXT_MAX];
    size_t completion_context_len;
    ev_mux_ticket_t *ticket_out;
} ev_control_response_context_t;

static ev_control_response_context_t s_response_contexts[EV_CONTROL_RESPONSE_CONTEXTS];
static ev_control_response_context_t s_response_fallback_context;
static portMUX_TYPE s_response_context_mux = portMUX_INITIALIZER_UNLOCKED;

static ev_control_response_context_t *ev_control_response_context_current(void)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (task == NULL) {
        return &s_response_fallback_context;
    }
    portENTER_CRITICAL(&s_response_context_mux);
    ev_control_response_context_t *free_context = NULL;
    ev_control_response_context_t *context = NULL;
    for (size_t i = 0; i < EV_CONTROL_RESPONSE_CONTEXTS; ++i) {
        if (s_response_contexts[i].task == task) {
            context = &s_response_contexts[i];
            break;
        }
        if ((free_context == NULL) && (s_response_contexts[i].task == NULL)) {
            free_context = &s_response_contexts[i];
        }
    }
    if ((context == NULL) && (free_context != NULL)) {
        context = free_context;
        memset(context, 0, sizeof(*context));
        context->task = task;
    }
    portEXIT_CRITICAL(&s_response_context_mux);
    return context != NULL ? context : &s_response_fallback_context;
}

#define s_response_rpc_domain (ev_control_response_context_current()->rpc_domain)
#define s_response_link (ev_control_response_context_current()->link)
#define s_has_response_link (ev_control_response_context_current()->has_link)
#define s_response_session_id (ev_control_response_context_current()->session_id)
#define s_response_vm_epoch (ev_control_response_context_current()->vm_epoch)
#define s_response_tx_vm_epoch (ev_control_response_context_current()->tx_vm_epoch)
#define s_response_input_generation (ev_control_response_context_current()->input_generation)
#define s_response_transition_id (ev_control_response_context_current()->transition_id)
#define s_response_completion (ev_control_response_context_current()->completion)
#define s_response_completion_context (ev_control_response_context_current()->completion_context)
#define s_response_completion_context_len (ev_control_response_context_current()->completion_context_len)
#define s_response_ticket_out (ev_control_response_context_current()->ticket_out)
static ev_control_vm_frame_t s_vm_queue[EV_CONTROL_VM_QUEUE_LEN];
static volatile uint8_t s_vm_queue_head;
static volatile uint8_t s_vm_queue_tail;
static StaticSemaphore_t s_vm_queue_lock_buf;
static SemaphoreHandle_t s_vm_queue_lock;
static StaticSemaphore_t s_transport_state_lock_buf;
static SemaphoreHandle_t s_transport_state_lock;
static TaskHandle_t s_transport_task;
static bool s_control_initialized;
#if MICROPY_HW_USB_CDC
static bool s_cdc_connection_known;
static bool s_cdc_mounted;
static uint32_t s_cdc_rx_generation;
#endif

static void ev_control_transport_task(void *arg);
static void ev_control_deliver_ctrl_c(const ev_link_ref_t *link,
                                      const char *session_id,
                                      uint32_t vm_epoch,
                                      uint32_t input_generation,
                                      uint32_t transition_id);
static void ev_control_session_changed(const ev_session_snapshot_t *snapshot,
                                       ev_session_change_reason_t reason,
                                       uint32_t transition_id,
                                       const ev_link_ref_t *previous_link,
                                       const char *previous_session_id);
static void ev_control_network_changed(const esp_vision_network_snapshot_t *snapshot, void *context);
static bool ev_control_json_get_string(const char *json, const char *key, char *out, size_t out_len);
static uint32_t ev_control_json_get_u32(const char *json, const char *key, uint32_t fallback);
static esp_err_t ev_control_send_error(const char *method, uint32_t seq, const char *code, const char *message);
static bool ev_control_rpc_needs_vm(ev_control_rpc_domain_t domain, const char *method, const char *payload);
#if MICROPY_HW_USB_CDC
static void ev_control_transport_cdc_update(void);
#endif

// Host-visible congestion/health counters (debug.info "transport.stats").
// Written mostly from the transport task, some from ISR contexts; 32-bit
// increments may rarely lose a count under races, which is acceptable here.
typedef struct {
    uint32_t rx_bytes[EV_TRANSPORT_MAX];
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t frames_rejected;
    uint32_t repl_rx_dropped;
    uint32_t vm_busy;
    uint32_t parser_timeouts;
    uint32_t stale_generation_drops;
} ev_transport_stats_t;

static ev_transport_stats_t s_stats;
// Producer-side ring-full events, incremented by the ingress drivers
// (overlay usb_serial_jtag.c / mp_usbd_cdc.c / uart.c).
volatile uint32_t ev_transport_rx_ring_full[EV_CONTROL_INGRESS_MAX];
static uint32_t ev_control_next_seq(void)
{
    portENTER_CRITICAL(&s_seq_mux);
    uint32_t seq = s_control_seq++;
    portEXIT_CRITICAL(&s_seq_mux);
    return seq;
}

static uint32_t ev_control_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void ev_control_parser_reset(ev_control_parser_t *parser)
{
    heap_caps_free(parser->file_payload);
    memset(parser, 0, sizeof(*parser));
    parser->state = EV_CONTROL_PARSE_SOF;
}

static void ev_control_vm_queue_lock(void)
{
    xSemaphoreTake(s_vm_queue_lock, portMAX_DELAY);
}

static void ev_control_vm_queue_unlock(void)
{
    xSemaphoreGive(s_vm_queue_lock);
}

static bool ev_control_repl_rx_push(const uint8_t *data,
                                    size_t len,
                                    const ev_link_ref_t *link,
                                    const char *session_id,
                                    uint32_t vm_epoch,
                                    uint32_t input_generation,
                                    uint32_t transition_id)
{
    if ((link == NULL) || (session_id == NULL) || (input_generation == 0)) {
        return false;
    }
    // Reauthorize immediately before entering the nonblocking ring critical
    // section. The atomic generation check below closes the pause-after-auth
    // race without taking the session mutex while holding portMUX.
    ev_session_snapshot_t current;
    if ((ev_session_authorize(link, session_id, true, true, vm_epoch,
                              ev_control_now_ms(), &current) != EV_SESSION_RESULT_OK) ||
            (current.transition_id != transition_id) ||
            (current.input_generation != input_generation)) {
        return false;
    }
    portENTER_CRITICAL(&s_repl_rx_mux);
    bool ok = ev_repl_queue_push(&s_repl_rx, data, len, input_generation,
                                 ev_session_input_generation());
    portEXIT_CRITICAL(&s_repl_rx_mux);
    return ok;
}

static void ev_control_repl_rx_clear(void)
{
    uint32_t generation = ev_session_input_generation();
    portENTER_CRITICAL(&s_repl_rx_mux);
    ev_repl_queue_force_clear(&s_repl_rx, generation);
    portEXIT_CRITICAL(&s_repl_rx_mux);
}

static bool ev_control_repl_rx_clear_generation(uint32_t generation)
{
    portENTER_CRITICAL(&s_repl_rx_mux);
    bool cleared = ev_repl_queue_clear_generation(&s_repl_rx, generation,
                                                  ev_session_input_generation());
    portEXIT_CRITICAL(&s_repl_rx_mux);
    return cleared;
}

static void ev_control_vm_queue_clear(void)
{
    ev_control_vm_queue_lock();
    memset(s_vm_queue, 0, sizeof(s_vm_queue));
    s_vm_queue_head = 0;
    s_vm_queue_tail = 0;
    ev_control_vm_queue_unlock();
}

static void ev_control_vm_queue_clear_stale(uint32_t input_generation)
{
    if (input_generation == 0) {
        return;
    }
    ev_control_vm_queue_lock();
    if (ev_session_input_generation() != input_generation) {
        ev_control_vm_queue_unlock();
        return;
    }
    if (s_vm_queue_head != s_vm_queue_tail) {
        ev_control_vm_frame_t *frame = &s_vm_queue[s_vm_queue_head];
        if (frame->input_generation != input_generation) {
            memset(frame, 0, sizeof(*frame));
            s_vm_queue_head = s_vm_queue_tail;
        }
    }
    ev_control_vm_queue_unlock();
}

static bool ev_control_repl_rx_pop(int *out_chr)
{
    if (out_chr == NULL) {
        return false;
    }
    if (ev_vm_output_input_origin_stale()) {
        // Do not consume a new owner's byte into an old edit/VM input source.
        // pyexec/readline or the raw reader handles Ctrl-C on its normal path.
        *out_chr = 3; // CHAR_CTRL_C
        return true;
    }
    ev_session_snapshot_t session;
    if (!ev_session_snapshot(&session)) {
        return false;
    }
    uint8_t value = 0;
    bool ready = (session.state == EV_SESSION_OWNED) && session.handshake_complete && session.vm_ready;
    if (ready) {
        ev_vm_output_origin_t origin;
        if (!ev_vm_output_origin_from_session(&session, &origin) ||
                !ev_vm_output_repl_input_accept(&origin)) {
            // Preserve the queued byte for the next readline after pyexec has
            // discarded the old generation's partial edit buffer.
            *out_chr = 3; // CHAR_CTRL_C
            return true;
        }
    }
    portENTER_CRITICAL(&s_repl_rx_mux);
    bool available = ev_repl_queue_pop(&s_repl_rx, session.input_generation,
                                       ev_session_input_generation(), ready, &value);
    portEXIT_CRITICAL(&s_repl_rx_mux);
    if (available) {
        *out_chr = value;
    }
    return available;
}

// Expire undispatched frames at the head of the queue before every push.
static void ev_control_vm_queue_expire(void)
{
    for (;;) {
        char method[EV_CONTROL_JSON_METHOD_MAX + 1];
        ev_control_rpc_domain_t domain;
        ev_link_ref_t link;
        char session_id[EV_SESSION_ID_HEX_LEN + 1];
        uint32_t vm_epoch;
        uint32_t input_generation;
        uint32_t transition_id;
        uint32_t seq;

        ev_control_vm_queue_lock();
        if (s_vm_queue_head == s_vm_queue_tail) {
            ev_control_vm_queue_unlock();
            return;
        }
        ev_control_vm_frame_t *frame = &s_vm_queue[s_vm_queue_head];
        if ((uint32_t)(ev_control_now_ms() - frame->queued_ms) <= EV_CONTROL_VM_EXPIRE_MS) {
            ev_control_vm_queue_unlock();
            return;
        }
        memcpy(method, frame->method, sizeof(method));
        seq = frame->seq;
        domain = (ev_control_rpc_domain_t)frame->domain;
        link = frame->link;
        memcpy(session_id, frame->session_id, sizeof(session_id));
        vm_epoch = frame->vm_epoch;
        input_generation = frame->input_generation;
        transition_id = frame->transition_id;
        s_vm_queue_head = (uint8_t)((s_vm_queue_head + 1) % EV_CONTROL_VM_QUEUE_LEN);
        ev_control_vm_queue_unlock();

        ev_session_snapshot_t snapshot;
        if ((ev_session_authorize(&link, session_id, true, true, vm_epoch,
                                  ev_control_now_ms(), &snapshot) != EV_SESSION_RESULT_OK) ||
                (snapshot.input_generation != input_generation) ||
                (snapshot.transition_id != transition_id)) {
            continue;
        }
        ev_control_rpc_domain_t prev_domain = s_response_rpc_domain;
        ev_link_ref_t prev_link = s_response_link;
        bool prev_has_link = s_has_response_link;
        char prev_session_id[sizeof(s_response_session_id)];
        memcpy(prev_session_id, s_response_session_id, sizeof(prev_session_id));
        uint32_t prev_vm_epoch = s_response_vm_epoch;
        uint32_t prev_tx_vm_epoch = s_response_tx_vm_epoch;
        uint32_t prev_input_generation = s_response_input_generation;
        uint32_t prev_transition_id = s_response_transition_id;
        s_response_rpc_domain = domain;
        s_response_link = link;
        s_has_response_link = true;
        memcpy(s_response_session_id, session_id, sizeof(s_response_session_id));
        s_response_vm_epoch = vm_epoch;
        s_response_tx_vm_epoch = vm_epoch;
        s_response_input_generation = input_generation;
        s_response_transition_id = transition_id;
        (void)ev_control_send_error(method, seq, "VM_TIMEOUT", "interpreter busy");
        s_response_rpc_domain = prev_domain;
        s_response_link = prev_link;
        s_has_response_link = prev_has_link;
        memcpy(s_response_session_id, prev_session_id, sizeof(s_response_session_id));
        s_response_vm_epoch = prev_vm_epoch;
        s_response_tx_vm_epoch = prev_tx_vm_epoch;
        s_response_input_generation = prev_input_generation;
        s_response_transition_id = prev_transition_id;
    }
}

static bool ev_control_vm_queue_push(ev_control_rpc_domain_t domain,
                                     const ev_link_ref_t *link,
                                     const char *session_id,
                                     const char *method,
                                     uint32_t seq,
                                     uint32_t vm_epoch,
                                     uint32_t input_generation,
                                     uint32_t transition_id,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    if ((method == NULL) || (strlen(method) > EV_CONTROL_JSON_METHOD_MAX)) {
        return false;
    }
    ev_session_snapshot_t current;
    if ((ev_session_authorize(link, session_id, true, true, vm_epoch,
                              ev_control_now_ms(), &current) != EV_SESSION_RESULT_OK) ||
            (current.input_generation != input_generation) ||
            (current.transition_id != transition_id)) {
        return false;
    }
    ev_control_vm_queue_expire();
    ev_control_vm_queue_lock();
    // The lockless generation is updated while session state changes. It
    // closes the authorization-to-enqueue race without inverting locks.
    if (ev_session_input_generation() != input_generation) {
        ev_control_vm_queue_unlock();
        return false;
    }
    uint8_t next_tail = (uint8_t)((s_vm_queue_tail + 1) % EV_CONTROL_VM_QUEUE_LEN);
    if (next_tail == s_vm_queue_head) {
        ev_control_vm_queue_unlock();
        return false;
    }
    ev_control_vm_frame_t *frame = &s_vm_queue[s_vm_queue_tail];
    frame->payload_len = (uint16_t)payload_len;
    frame->domain = (uint8_t)domain;
    frame->link = *link;
    memcpy(frame->session_id, session_id, EV_SESSION_ID_HEX_LEN + 1);
    frame->vm_epoch = vm_epoch;
    frame->input_generation = input_generation;
    frame->transition_id = transition_id;
    frame->queued_ms = ev_control_now_ms();
    frame->seq = seq;
    memcpy(frame->method, method, strlen(method) + 1);
    memcpy(frame->payload, payload, payload_len);
    frame->payload[payload_len] = '\0';
    s_vm_queue_tail = next_tail;
    ev_control_vm_queue_unlock();
    return true;
}

static const char *ev_control_json_find_value(const char *json, const char *key)
{
    char pattern[40];
    int len = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if ((len <= 0) || ((size_t)len >= sizeof(pattern))) {
        return NULL;
    }

    const char *pos = strstr(json, pattern);
    if (pos == NULL) {
        return NULL;
    }
    pos += len;
    while ((*pos != '\0') && isspace((unsigned char) * pos)) {
        ++pos;
    }
    if (*pos != ':') {
        return NULL;
    }
    ++pos;
    while ((*pos != '\0') && isspace((unsigned char) * pos)) {
        ++pos;
    }
    return pos;
}

static bool ev_control_json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    if ((out == NULL) || (out_len == 0)) {
        return false;
    }
    const char *pos = ev_control_json_find_value(json, key);
    if ((pos == NULL) || (*pos != '"')) {
        return false;
    }
    ++pos;

    size_t len = 0;
    while ((*pos != '\0') && (*pos != '"') && (len + 1 < out_len)) {
        out[len++] = *pos++;
    }
    out[len] = '\0';
    return *pos == '"';
}

static bool ev_control_json_get_bool(const char *json, const char *key, bool fallback)
{
    const char *pos = ev_control_json_find_value(json, key);
    if (pos == NULL) {
        return fallback;
    }
    if ((strncmp(pos, "true", 4) == 0) &&
            ((pos[4] == '\0') || (pos[4] == ',') || (pos[4] == '}') || isspace((unsigned char)pos[4]))) {
        return true;
    }
    if ((strncmp(pos, "false", 5) == 0) &&
            ((pos[5] == '\0') || (pos[5] == ',') || (pos[5] == '}') || isspace((unsigned char)pos[5]))) {
        return false;
    }
    return fallback;
}

static uint32_t ev_control_json_get_u32(const char *json, const char *key, uint32_t fallback)
{
    const char *pos = ev_control_json_find_value(json, key);
    if (pos == NULL) {
        return fallback;
    }
    return (uint32_t)strtoul(pos, NULL, 10);
}

static void ev_control_json_escape(char *out, size_t out_len, const char *in)
{
    if ((out == NULL) || (out_len == 0)) {
        return;
    }
    size_t pos = 0;
    for (const char *p = in; (*p != '\0') && (pos + 1 < out_len); ++p) {
        char c = *p;
        if ((c == '\\') || (c == '"')) {
            if (pos + 2 >= out_len) {
                break;
            }
            out[pos++] = '\\';
            out[pos++] = c;
        } else if ((unsigned char)c < 0x20) {
            if (pos + 6 >= out_len) {
                break;
            }
            pos += snprintf(out + pos, out_len - pos, "\\u%04x", (unsigned int)(unsigned char)c);
        } else {
            out[pos++] = c;
        }
    }
    out[pos] = '\0';
}

static bool ev_control_path_allowed(const char *path)
{
    if ((path == NULL) || (path[0] == '\0')) {
        return false;
    }
    if (strlen(path) > EV_CONTROL_PATH_MAX) {
        return false;
    }
    if (strstr(path, "..") != NULL) {
        return false;
    }
    if (strstr(path, "//") != NULL) {
        return false;
    }
    // Paths are embedded in a double-quoted REPL command by script.run.
    // Reject characters that could escape that literal or inject a command.
    for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; ++p) {
        if ((*p < 0x20) || (*p == 0x7f) || (*p == '\\') || (*p == '"')) {
            return false;
        }
    }
    return true;
}

static bool ev_control_path_too_long(const char *path)
{
    return (path != NULL) && (strlen(path) > EV_CONTROL_PATH_MAX);
}

static int ev_control_base64_value(char c)
{
    if ((c >= 'A') && (c <= 'Z')) {
        return c - 'A';
    }
    if ((c >= 'a') && (c <= 'z')) {
        return c - 'a' + 26;
    }
    if ((c >= '0') && (c <= '9')) {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static esp_err_t ev_control_base64_decode(const char *src, uint8_t *dst, size_t dst_len, size_t *out_len)
{
    size_t src_len = strlen(src);
    size_t pos = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < src_len; ++i) {
        char c = src[i];
        if (c == '=') {
            break;
        }
        if (isspace((unsigned char)c)) {
            continue;
        }
        int value = ev_control_base64_value(c);
        if (value < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        acc = (acc << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (pos >= dst_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            dst[pos++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    *out_len = pos;
    return ESP_OK;
}

static const char s_base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static esp_err_t ev_control_base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len)
{
    size_t out_len = ((src_len + 2) / 3) * 4;
    if (dst_len <= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pos = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        uint32_t value = (uint32_t)src[i] << 16;
        bool have_1 = (i + 1) < src_len;
        bool have_2 = (i + 2) < src_len;
        if (have_1) {
            value |= (uint32_t)src[i + 1] << 8;
        }
        if (have_2) {
            value |= src[i + 2];
        }
        dst[pos++] = s_base64_table[(value >> 18) & 0x3f];
        dst[pos++] = s_base64_table[(value >> 12) & 0x3f];
        dst[pos++] = have_1 ? s_base64_table[(value >> 6) & 0x3f] : '=';
        dst[pos++] = have_2 ? s_base64_table[value & 0x3f] : '=';
    }
    dst[pos] = '\0';
    return ESP_OK;
}

static const char *ev_control_bool_str(bool value)
{
    return value ? "true" : "false";
}

static esp_err_t ev_control_vfs_exists(const char *path, bool *exists)
{
    if (exists == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *exists = false;

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        *exists = mp_vfs_import_stat(path) != MP_IMPORT_STAT_NO_EXIST;
        nlr_pop();
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t ev_control_vfs_remove_if_exists(const char *path)
{
    bool exists = false;
    if (ev_control_vfs_exists(path, &exists) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!exists) {
        return ESP_OK;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t path_obj = mp_obj_new_str_from_cstr(path);
        (void)mp_vfs_remove(path_obj);
        nlr_pop();
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t ev_control_vfs_close(mp_obj_t file)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t close_method[2];
        mp_load_method(file, MP_QSTR_close, close_method);
        (void)mp_call_method_n_kw(0, 0, close_method);
        nlr_pop();
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t ev_control_vfs_write_file_mode(const char *path, const uint8_t *data, size_t len, const char *mode)
{
    mp_obj_t file = MP_OBJ_NULL;

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t args[2] = {
            mp_obj_new_str_from_cstr(path),
            mp_obj_new_str_from_cstr(mode),
        };
        file = mp_vfs_open(2, args, (mp_map_t *)&mp_const_empty_map);
        int err = 0;
        mp_uint_t written = mp_stream_rw(file, (void *)data, len, &err, MP_STREAM_RW_WRITE);
        nlr_pop();
        if ((err != 0) || (written != len)) {
            (void)ev_control_vfs_close(file);
            return ESP_FAIL;
        }
        return ev_control_vfs_close(file);
    }

    if (file != MP_OBJ_NULL) {
        (void)ev_control_vfs_close(file);
    }
    return ESP_FAIL;
}

static esp_err_t ev_control_vfs_write_file(const char *path, const uint8_t *data, size_t len)
{
    return ev_control_vfs_write_file_mode(path, data, len, "wb");
}

static esp_err_t ev_control_vfs_append_file(const char *path, const uint8_t *data, size_t len)
{
    return ev_control_vfs_write_file_mode(path, data, len, "ab");
}

static esp_err_t ev_control_vfs_read_file(const char *path, uint8_t *data, size_t data_len, size_t *out_len)
{
    mp_obj_t file = MP_OBJ_NULL;

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t args[2] = {
            mp_obj_new_str_from_cstr(path),
            mp_obj_new_str_from_cstr("rb"),
        };
        file = mp_vfs_open(2, args, (mp_map_t *)&mp_const_empty_map);
        int err = 0;
        *out_len = mp_stream_rw(file, data, data_len, &err, MP_STREAM_RW_READ | MP_STREAM_RW_ONCE);
        nlr_pop();
        if (err != 0) {
            (void)ev_control_vfs_close(file);
            return ESP_FAIL;
        }
        return ev_control_vfs_close(file);
    }

    if (file != MP_OBJ_NULL) {
        (void)ev_control_vfs_close(file);
    }
    return ESP_FAIL;
}

static esp_err_t ev_control_vfs_rename(const char *old_path, const char *new_path)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t old_path_obj = mp_obj_new_str_from_cstr(old_path);
        mp_obj_t new_path_obj = mp_obj_new_str_from_cstr(new_path);
        (void)mp_vfs_rename(old_path_obj, new_path_obj);
        nlr_pop();
        return ESP_OK;
    }
    return ESP_FAIL;
}

static ev_stream_t ev_control_stream_from_name(const char *name)
{
    if (strcmp(name, "user") == 0) {
        return EV_STREAM_USER;
    }
    if (strcmp(name, "debug") == 0) {
        return EV_STREAM_DEBUG;
    }
    return EV_STREAM_MAX;
}

static const char *ev_control_stream_name(ev_stream_t stream)
{
    switch (stream) {
    case EV_STREAM_USER:
        return "user";
    case EV_STREAM_DEBUG:
        return "debug";
    default:
        return "unknown";
    }
}

static const char *ev_control_rpc_channel(ev_control_rpc_domain_t domain)
{
    return (domain == EV_RPC_DEBUG) ? "debug.rpc" : "user.rpc";
}

static ev_stream_t ev_control_rpc_stream(ev_control_rpc_domain_t domain)
{
    return (domain == EV_RPC_DEBUG) ? EV_STREAM_DEBUG : EV_STREAM_USER;
}

enum {
    EV_CONTROL_TX_DONE_HELLO = 1,
    EV_CONTROL_TX_DONE_RELEASE,
    EV_CONTROL_TX_DONE_NETWORK,
    EV_CONTROL_TX_DONE_CLOSE_LINK,
};

typedef struct {
    uint8_t kind;
    char value[EV_SESSION_ID_HEX_LEN + 1];
} ev_control_tx_context_t;

static void ev_control_tx_complete(esp_err_t result,
                                   ev_link_ref_t link,
                                   const void *context,
                                   size_t context_len)
{
    if ((context == NULL) || (context_len < sizeof(ev_control_tx_context_t))) {
        return;
    }
    const ev_control_tx_context_t *tx_context = (const ev_control_tx_context_t *)context;
    switch (tx_context->kind) {
    case EV_CONTROL_TX_DONE_HELLO:
        (void)ev_session_hello_complete(&link, tx_context->value, result == ESP_OK);
        break;
    case EV_CONTROL_TX_DONE_RELEASE:
        (void)ev_mux_cancel_session(&link, tx_context->value);
        (void)ev_session_release_finish(&link, tx_context->value, EV_SESSION_CHANGE_RELEASED);
        break;
    case EV_CONTROL_TX_DONE_NETWORK:
        if (result == ESP_OK) {
            esp_vision_network_rpc_response_sent(tx_context->value);
        }
        break;
    case EV_CONTROL_TX_DONE_CLOSE_LINK:
        ev_transport_link_close(&link);
        break;
    default:
        break;
    }
}

static bool ev_control_set_completion(uint8_t kind, const char *value)
{
    if ((kind == 0) || (value == NULL) || (strlen(value) > EV_SESSION_ID_HEX_LEN)) {
        return false;
    }
    ev_control_tx_context_t tx_context = { .kind = kind };
    memcpy(tx_context.value, value, strlen(value) + 1);
    memcpy(s_response_completion_context, &tx_context, sizeof(tx_context));
    s_response_completion_context_len = sizeof(tx_context);
    s_response_completion = ev_control_tx_complete;
    return true;
}

static ev_mux_tx_class_t ev_control_tx_class(const char *type, const char *method)
{
    if (strcmp(type, "event") == 0) {
        return EV_MUX_TX_SESSION;
    }
    if ((strcmp(method, "hello") == 0) || (strcmp(method, "capabilities") == 0) ||
            (strcmp(method, "session.keepalive") == 0) || (strcmp(method, "session.release") == 0)) {
        return EV_MUX_TX_SESSION;
    }
    return EV_MUX_TX_CONTROL;
}

static esp_err_t ev_control_submit_rpc_link(const ev_link_ref_t *link,
                                            ev_control_rpc_domain_t domain,
                                            const char *type,
                                            const char *method,
                                            const char *payload,
                                            uint32_t seq,
                                            const char *session_id,
                                            uint32_t vm_epoch,
                                            uint32_t tx_vm_epoch,
                                            ev_mux_ticket_t *ticket_out)
{
    if ((link == NULL) || (type == NULL) || (method == NULL) || (payload == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    char metadata[EV_CONTROL_METADATA_MAX];
    int metadata_len;
    char session_json[EV_SESSION_ID_HEX_LEN + 3];
    char epoch_json[16];
    if (session_id != NULL) {
        int n = snprintf(session_json, sizeof(session_json), "\"%s\"", session_id);
        if ((n <= 0) || ((size_t)n >= sizeof(session_json))) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        strcpy(session_json, "null");
    }
    if (session_id != NULL) {
        int n = snprintf(epoch_json, sizeof(epoch_json), "%" PRIu32, vm_epoch);
        if ((n <= 0) || ((size_t)n >= sizeof(epoch_json))) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        strcpy(epoch_json, "null");
    }
    if (session_id != NULL) {
        metadata_len = snprintf(metadata,
                                sizeof(metadata),
                                "{\"sid\":\"%s\",\"seq\":%" PRIu32 ",\"channel\":\"%s\","
                                "\"type\":\"%s\",\"method\":\"%s\",\"encoding\":\"json\","
                                "\"sessionId\":%s,\"vmEpoch\":%s,\"ts_ms\":%" PRIu32 "}",
                                (domain == EV_RPC_DEBUG) ? "debug" : "user",
                                seq,
                                ev_control_rpc_channel(domain),
                                type,
                                method,
                                session_json,
                                epoch_json,
                                ev_control_now_ms());
    } else {
        metadata_len = snprintf(metadata,
                                sizeof(metadata),
                                "{\"sid\":\"%s\",\"seq\":%" PRIu32 ",\"channel\":\"%s\","
                                "\"type\":\"%s\",\"method\":\"%s\",\"encoding\":\"json\","
                                "\"ts_ms\":%" PRIu32 "}",
                                (domain == EV_RPC_DEBUG) ? "debug" : "user",
                                seq,
                                ev_control_rpc_channel(domain),
                                type,
                                method,
                                ev_control_now_ms());
    }
    if ((metadata_len <= 0) || ((size_t)metadata_len >= sizeof(metadata))) {
        return ESP_ERR_INVALID_SIZE;
    }

    ev_mux_submit_options_t options = {
        .link = link,
        .session_id = session_id,
        .vm_epoch = tx_vm_epoch,
        .tx_class = ev_control_tx_class(type, method),
        .completion = s_response_completion,
        .completion_context = s_response_completion_context_len ? s_response_completion_context : NULL,
        .completion_context_len = s_response_completion_context_len,
    };
    s_response_completion = NULL;
    s_response_completion_context_len = 0;
    return ev_mux_submit_link(&options, metadata, payload, strlen(payload), ticket_out);
}

static esp_err_t ev_control_send_rpc_with_domain(ev_control_rpc_domain_t domain,
                                                 const char *type,
                                                 const char *method,
                                                 const char *payload,
                                                 uint32_t seq)
{
    ev_link_ref_t target;
    const char *session_id = NULL;
    uint32_t vm_epoch = 0;
    uint32_t tx_vm_epoch = 0;
    // Keep the event's session storage alive until submit has copied it.
    ev_session_snapshot_t snapshot;
    if (strcmp(type, "rsp") == 0) {
        if (!s_has_response_link) {
            return ESP_ERR_INVALID_STATE;
        }
        target = s_response_link;
        if (s_response_session_id[0] != '\0') {
            session_id = s_response_session_id;
            vm_epoch = s_response_vm_epoch;
            tx_vm_epoch = s_response_tx_vm_epoch;
        }
    } else {
        if (!ev_channel_target_link(ev_control_rpc_stream(domain), &target)) {
            return ESP_ERR_NOT_FOUND;
        }
        if (ev_session_snapshot(&snapshot) && snapshot.has_owner &&
                snapshot.owner_link.transport_id == target.transport_id &&
                snapshot.owner_link.slot == target.slot &&
                snapshot.owner_link.generation == target.generation) {
            session_id = snapshot.session_id;
            vm_epoch = snapshot.vm_epoch;
        }
        // The VM-ready event is epoch-scoped. Session/route/network events
        // remain valid across a VM restart and use an unscoped TX record.
        if ((strcmp(type, "event") == 0) && (strcmp(method, "hello") == 0)) {
            tx_vm_epoch = ev_session_vm_epoch();
        }
    }
    return ev_control_submit_rpc_link(&target, domain, type, method, payload, seq,
                                      session_id, vm_epoch, tx_vm_epoch, s_response_ticket_out);
}

static esp_err_t ev_control_send_rpc_with_seq(const char *type,
                                              const char *method,
                                              const char *payload,
                                              uint32_t seq)
{
    return ev_control_send_rpc_with_domain(s_response_rpc_domain, type, method, payload, seq);
}

// Binary capture RPC response: the payload travels inside the
// debug.rpc response frame itself — there is no separate data channel. Sent
// to the sink the request arrived on, independent of stream routing, so a
// CDC-originated request never strands its image.
static esp_err_t ev_control_send_binary_rsp(const char *method,
                                            const char *content_type,
                                            const void *payload,
                                            size_t payload_len,
                                            uint32_t seq,
                                            int32_t width,
                                            int32_t height)
{
    if (!s_has_response_link) {
        return ESP_ERR_INVALID_STATE;
    }
    char metadata[EV_CONTROL_METADATA_MAX];
    int metadata_len = snprintf(metadata,
                                sizeof(metadata),
                                "{\"sid\":\"debug\",\"seq\":%" PRIu32 ",\"channel\":\"debug.rpc\","
                                "\"type\":\"rsp\",\"method\":\"%s\",\"encoding\":\"binary\","
                                "\"contentType\":\"%s\",\"width\":%" PRIi32 ",\"height\":%" PRIi32 ","
                                "\"sessionId\":\"%s\",\"vmEpoch\":%" PRIu32 ",\"ts_ms\":%" PRIu32 "}",
                                seq,
                                method,
                                content_type,
                                width,
                                height,
                                s_response_session_id,
                                s_response_vm_epoch,
                                ev_control_now_ms());
    if ((metadata_len <= 0) || ((size_t)metadata_len >= sizeof(metadata))) {
        return ESP_ERR_INVALID_SIZE;
    }
    ev_mux_submit_options_t options = {
        .link = &s_response_link,
        .session_id = s_response_session_id,
        .vm_epoch = s_response_tx_vm_epoch,
        .tx_class = EV_MUX_TX_CONTROL,
    };
    return ev_mux_submit_large_link(&options, metadata, payload, payload_len, NULL);
}

static esp_err_t ev_control_send_error(const char *method, uint32_t seq, const char *code, const char *message)
{
    char payload[256];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                       code,
                       message);
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", method, payload, seq);
}

static const char *ev_control_session_change_reason(ev_session_change_reason_t reason)
{
    switch (reason) {
    case EV_SESSION_CHANGE_ACQUIRED: return "session_acquired";
    case EV_SESSION_CHANGE_RELEASED: return "session_released";
    case EV_SESSION_CHANGE_EXPIRED: return "session_expired";
    case EV_SESSION_CHANGE_LINK_LOST: return "link_lost";
    case EV_SESSION_CHANGE_TX_FAILED: return "tx_failed";
    default: return "session_released";
    }
}

static void ev_control_session_changed(const ev_session_snapshot_t *snapshot,
                                       ev_session_change_reason_t reason,
                                       uint32_t transition_id,
                                       const ev_link_ref_t *previous_link,
                                       const char *previous_session_id)
{
    if (snapshot == NULL) {
        return;
    }
    ev_session_snapshot_t current;
    if (!ev_session_snapshot(&current) || (current.transition_id != transition_id) ||
            (current.input_generation != snapshot->input_generation)) {
        return;
    }
    /* A lease is bound to the authenticated EV-MUX session. Release it only
     * through the explicit storage.writer.release action; any other session
     * transition permanently quarantines an outstanding lease. */
    ev_file_transfer_poll();
    esp_vision_msc_writer_session_changed(snapshot->has_owner ? snapshot->session_id : NULL);
    if ((previous_link != NULL) && (previous_session_id != NULL) &&
            (previous_session_id[0] != '\0')) {
        ev_mux_cancel_session(previous_link, previous_session_id);
    }
    // A transition invalidates old owner input on both release and acquire.
    // Generation-conditional clearing preserves data from a newer callback
    // if this one was delayed after its initial transition check.
    ev_control_vm_queue_clear_stale(snapshot->input_generation);
    (void)ev_control_repl_rx_clear_generation(snapshot->input_generation);
    if (!ev_session_snapshot(&current) || (current.transition_id != transition_id) ||
            (current.input_generation != snapshot->input_generation)) {
        return;
    }
    const char *route = snapshot->has_owner ? ev_transport_name(snapshot->owner_link.transport_id) :
                        ev_channel_get(EV_STREAM_USER);
    char session_id[EV_SESSION_ID_HEX_LEN + 3] = "null";
    if (snapshot->has_owner) {
        int session_len = snprintf(session_id, sizeof(session_id), "\"%s\"", snapshot->session_id);
        if ((session_len <= 0) || ((size_t)session_len >= sizeof(session_id))) {
            return;
        }
    }
    char payload[224];
    for (ev_stream_t stream = EV_STREAM_USER; stream < EV_STREAM_MAX; ++stream) {
        int len = snprintf(payload,
                           sizeof(payload),
                           "{\"ok\":true,\"stream\":\"%s\",\"route\":\"%s\","
                           "\"reason\":\"%s\",\"sessionId\":%s,\"transitionId\":%" PRIu32 "}",
                           ev_control_stream_name(stream),
                           route,
                           ev_control_session_change_reason(reason),
                           session_id,
                           transition_id);
        if ((len > 0) && ((size_t)len < sizeof(payload))) {
            (void)ev_control_send_rpc_with_domain(EV_RPC_USER, "event", "route.changed",
                                                  payload, ev_control_next_seq());
        }
    }
}

static const char *ev_control_network_state_name(esp_vision_network_state_t state)
{
    switch (state) {
    case ESP_VISION_NETWORK_UNSUPPORTED: return "unsupported";
    case ESP_VISION_NETWORK_UNCONFIGURED: return "unconfigured";
    case ESP_VISION_NETWORK_DISABLED: return "disabled";
    case ESP_VISION_NETWORK_CONNECTING: return "connecting";
    case ESP_VISION_NETWORK_ONLINE: return "online";
    case ESP_VISION_NETWORK_RETRY_WAIT: return "retry_wait";
    default: return "unsupported";
    }
}

static const char *ev_control_network_operation_state_name(esp_vision_network_operation_state_t state)
{
    switch (state) {
    case ESP_VISION_NETWORK_OPERATION_QUEUED: return "queued";
    case ESP_VISION_NETWORK_OPERATION_RUNNING: return "running";
    case ESP_VISION_NETWORK_OPERATION_SUCCEEDED: return "succeeded";
    case ESP_VISION_NETWORK_OPERATION_FAILED: return "failed";
    case ESP_VISION_NETWORK_OPERATION_NONE:
    default: return "unknown";
    }
}

static const char *ev_control_network_action_name(esp_vision_network_action_t action)
{
    switch (action) {
    case ESP_VISION_NETWORK_ACTION_CONFIGURE: return "network.configure";
    case ESP_VISION_NETWORK_ACTION_ENABLE: return "network.enable";
    case ESP_VISION_NETWORK_ACTION_FORGET: return "network.forget";
    case ESP_VISION_NETWORK_ACTION_NONE:
    default: return "unknown";
    }
}

static void ev_control_network_changed(const esp_vision_network_snapshot_t *snapshot, void *context)
{
    (void)context;
    if (snapshot == NULL) {
        return;
    }
    const esp_vision_network_operation_t *operation = snapshot->has_operation ?
                                                      &snapshot->operation : (snapshot->has_last_operation ? &snapshot->last_operation : NULL);
    char operation_id[35] = "null";
    const char *action = "unknown";
    const char *operation_state = "unknown";
    if (operation != NULL) {
        if (operation->id[0] != '\0') {
            int id_len = snprintf(operation_id, sizeof(operation_id), "\"%s\"", operation->id);
            if ((id_len <= 0) || ((size_t)id_len >= sizeof(operation_id))) {
                return;
            }
        }
        action = ev_control_network_action_name(operation->action);
        operation_state = ev_control_network_operation_state_name(operation->state);
    }
    char payload[256];
    int len = snprintf(payload, sizeof(payload),
                       "{\"ok\":true,\"operationId\":%s,\"action\":\"%s\","
                       "\"operationState\":\"%s\",\"networkState\":\"%s\","
                       "\"configRevision\":%" PRIu32 "}",
                       operation_id, action, operation_state,
                       ev_control_network_state_name(snapshot->state), snapshot->config_revision);
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return;
    }
    (void)ev_control_send_rpc_with_domain(EV_RPC_USER, "event", "network.changed",
                                          payload, ev_control_next_seq());
}

static esp_err_t ev_control_send_hello_event(uint32_t seq)
{
    ev_session_snapshot_t session;
    if (!ev_session_snapshot(&session)) {
        return ESP_ERR_INVALID_STATE;
    }
    // Do not advertise the newly allocated session until its acquire response
    // has reached the host. The completion callback is the ownership boundary.
    if (session.has_owner && !session.handshake_complete) {
        return ESP_ERR_INVALID_STATE;
    }
    char payload[768];
    int len;
    if (session.has_owner) {
        len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"device\":\"esp-vision\","
                       "\"deviceId\":\"%s\",\"hostname\":\"%s\","
                       "\"firmware\":{\"id\":\"%s\",\"version\":\"%s\"},"
                       "\"board\":\"%s\",\"arch\":\"%s\",\"idf\":\"%s\","
                       "\"evMuxVersion\":4,\"session\":{\"id\":\"%s\",\"owner\":\"%s\","
                       "\"leaseMs\":%u,\"keepaliveMs\":%u,\"vmEpoch\":%" PRIu32 "},"
                       "\"vmEpoch\":%" PRIu32 ",\"vmReady\":true}",
                       esp_vision_device_id(), esp_vision_hostname(),
                       ESP_VISION_FIRMWARE_ID, ESP_VISION_FIRMWARE_VERSION,
                       ESP_VISION_BOARD_TYPE, ESP_VISION_BOARD_ARCH, esp_get_idf_version(),
                       session.session_id, ev_transport_name(session.owner_link.transport_id),
                       (unsigned int)EV_SESSION_LEASE_MS, (unsigned int)EV_SESSION_KEEPALIVE_MS,
                       session.vm_epoch, session.vm_epoch);
    } else {
        len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"device\":\"esp-vision\","
                       "\"deviceId\":\"%s\",\"hostname\":\"%s\","
                       "\"firmware\":{\"id\":\"%s\",\"version\":\"%s\"},"
                       "\"board\":\"%s\",\"arch\":\"%s\",\"idf\":\"%s\","
                       "\"evMuxVersion\":4,\"session\":null,\"vmEpoch\":%" PRIu32 ",\"vmReady\":true}",
                       esp_vision_device_id(), esp_vision_hostname(),
                       ESP_VISION_FIRMWARE_ID, ESP_VISION_FIRMWARE_VERSION,
                       ESP_VISION_BOARD_TYPE, ESP_VISION_BOARD_ARCH, esp_get_idf_version(),
                       session.vm_epoch);
    }
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_domain(EV_RPC_USER, "event", "hello", payload, seq);
}

esp_err_t ev_control_transport_send_hello(void)
{
    if (!ev_session_vm_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    return ev_control_send_hello_event(ev_control_next_seq());
}

static const char *ev_control_json_nullable_bool(bool known, bool value)
{
    return !known ? "null" : (value ? "true" : "false");
}

static int ev_control_append_sink(char *payload, size_t capacity, int *pos, bool first,
                                  ev_transport_id_t id, const char *kind)
{
    ev_transport_endpoint_state_t state;
    ev_transport_endpoint_state(id, EV_TRANSPORT_PHYSICAL_SLOT, &state);
    char tx_timeout[112] = "";
    if (id == EV_TRANSPORT_UART) {
        (void)snprintf(tx_timeout, sizeof(tx_timeout),
                       ",\"txTimeout\":{\"baseMs\":1500,\"bitsPerByte\":10,\"factor\":2,"
                       "\"marginMs\":250,\"maxMs\":10000,\"baud\":%u}",
                       (unsigned int)MICROPY_HW_UART_REPL_BAUD);
    }
    int n = snprintf(payload + *pos,
                     capacity - (size_t) * pos,
                     "%s{\"id\":\"%s\",\"kind\":\"%s\",\"supported\":%s,"
                     "\"state\":\"%s\",\"present\":%s,\"connected\":%s,\"ready\":%s,\"owner\":%s%s}",
                     first ? "" : ",",
                     ev_transport_name(id), kind,
                     ev_control_bool_str(state.supported),
                     ev_channel_sink_state(ev_transport_name(id)),
                     ev_control_json_nullable_bool(state.present_known, state.present),
                     ev_control_json_nullable_bool(state.connected_known, state.connected),
                     ev_control_bool_str(state.ready),
                     ev_control_bool_str(state.owner),
                     tx_timeout);
    if ((n <= 0) || ((size_t)n >= capacity - (size_t) * pos)) {
        return -1;
    }
    *pos += n;
    return 0;
}

static esp_err_t ev_control_send_capabilities(uint32_t seq)
{
    char payload[3072];
    int pos = snprintf(payload, sizeof(payload),
                       "{\"ok\":true,\"device\":\"esp-vision\","
                       "\"deviceId\":\"%s\",\"hostname\":\"%s\","
                       "\"firmware\":{\"id\":\"%s\",\"version\":\"%s\"},"
                       "\"board\":\"%s\",\"arch\":\"%s\",\"idf\":\"%s\","
                       "\"protocol\":{\"evMuxVersion\":4},\"usbProduct\":\"%s\","
                       "\"defaultSink\":\"%s\",\"fallbackSink\":\"%s\",\"sinks\":[",
                       esp_vision_device_id(), esp_vision_hostname(),
                       ESP_VISION_FIRMWARE_ID, ESP_VISION_FIRMWARE_VERSION,
                       ESP_VISION_BOARD_TYPE, ESP_VISION_BOARD_ARCH, esp_get_idf_version(),
                       MICROPY_HW_USB_PRODUCT_FS_STRING,
                       ESP_VISION_TRANSPORT_DEFAULT_SINK,
                       ESP_VISION_TRANSPORT_FALLBACK_SINK);
    if ((pos <= 0) || ((size_t)pos >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    bool first_sink = true;
    for (ev_transport_id_t id = EV_TRANSPORT_USJ; id < EV_TRANSPORT_MAX; ++id) {
        const char *kind = (id == EV_TRANSPORT_USJ) ? "usb_serial_jtag" :
                           (id == EV_TRANSPORT_USB) ? "usb_otg_cdc" :
                           (id == EV_TRANSPORT_UART) ? "uart" : "tcp";
        if (ev_control_append_sink(payload, sizeof(payload), &pos, first_sink, id, kind) != 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        first_sink = false;
    }
    bool tcp_supported = false;
#if defined(ESP_VISION_ENABLE_TCP_TRANSPORT) && ESP_VISION_ENABLE_TCP_TRANSPORT
    tcp_supported = true;
#endif
    int n = snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                     "],\"streams\":[\"user\",\"debug\"],"
                     "\"channels\":[\"user.rpc\",\"debug.rpc\",\"repl.stdin\",\"repl.stdout\","
                     "\"repl.stderr\",\"repl.signal\",\"log.idf\",\"preview.frame\"],"
                     "\"features\":[\"hello\",\"capabilities\",\"transport.state\",\"route.get\","
                     "\"route.changed\",\"session.firstCome\",\"session.keepalive\","
                     "\"network.background\",\"network.configure\",\"framed.repl\","
                     "\"file.write.binary\",\"script.write\",\"script.write.chunked\",\"script.run\","
                     "\"launcher.boot.status.v1\",\"launcher.trial.status.v1\",\"launcher.typed-runner.v1\","
#if MICROPY_HW_ENABLE_USBDEV && MICROPY_HW_USB_MSC
                     "\"debug.info\",\"debug.capture_frame\",\"debug.capture_screen\",\"device.control\",\"storage.writer.lease\"],"
#else
                     "\"debug.info\",\"debug.capture_frame\",\"debug.capture_screen\",\"device.control\"],"
#endif
                     "\"limits\":{\"metadataMax\":384,\"rxPayloadMax\":2048,"
                     "\"fileWriteChunkMax\":16384,\"replInputMax\":511,\"vmPendingMax\":1,\"txLargePayloadMax\":%u,"
                     "\"txFrameTimeoutMs\":1500,\"txQueueTimeoutMs\":2000},"
                     "\"session\":{\"policy\":\"firstCome\",\"leaseMs\":%u,\"keepaliveMs\":%u},"
                     "\"tcp\":{\"supported\":%s,\"port\":%u,\"maxLinks\":2,\"handshakeTimeoutMs\":3000}}",
                     (unsigned int)EV_MUX_LARGE_PAYLOAD_MAX,
                     (unsigned int)EV_SESSION_LEASE_MS,
                     (unsigned int)EV_SESSION_KEEPALIVE_MS,
                     ev_control_bool_str(tcp_supported),
                     (unsigned int)ESP_VISION_TCP_PORT);
    if ((n <= 0) || ((size_t)n >= sizeof(payload) - (size_t)pos)) {
        return ESP_ERR_INVALID_SIZE;
    }
    pos += n;
    return ev_control_send_rpc_with_seq("rsp", "capabilities", payload, seq);
}

static esp_err_t ev_control_build_hello_discovery_payload(char *payload, size_t capacity)
{
    if ((payload == NULL) || (capacity == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    ev_session_snapshot_t session;
    if (!ev_session_snapshot(&session)) {
        return ESP_ERR_INVALID_STATE;
    }
    int len;
    if (session.has_owner) {
        len = snprintf(payload,
                       capacity,
                       "{\"ok\":true,\"device\":\"esp-vision\","
                       "\"deviceId\":\"%s\",\"hostname\":\"%s\","
                       "\"firmware\":{\"id\":\"%s\",\"version\":\"%s\"},"
                       "\"board\":\"%s\",\"arch\":\"%s\",\"idf\":\"%s\","
                       "\"evMuxVersion\":4,\"vmReady\":%s,"
                       "\"session\":{\"occupied\":true,\"owner\":\"%s\"}}",
                       esp_vision_device_id(), esp_vision_hostname(),
                       ESP_VISION_FIRMWARE_ID, ESP_VISION_FIRMWARE_VERSION,
                       ESP_VISION_BOARD_TYPE, ESP_VISION_BOARD_ARCH, esp_get_idf_version(),
                       ev_control_bool_str(session.vm_ready),
                       ev_transport_name(session.owner_link.transport_id));
    } else {
        len = snprintf(payload,
                       capacity,
                       "{\"ok\":true,\"device\":\"esp-vision\","
                       "\"deviceId\":\"%s\",\"hostname\":\"%s\","
                       "\"firmware\":{\"id\":\"%s\",\"version\":\"%s\"},"
                       "\"board\":\"%s\",\"arch\":\"%s\",\"idf\":\"%s\","
                       "\"evMuxVersion\":4,\"vmReady\":%s,\"session\":null}",
                       esp_vision_device_id(), esp_vision_hostname(),
                       ESP_VISION_FIRMWARE_ID, ESP_VISION_FIRMWARE_VERSION,
                       ESP_VISION_BOARD_TYPE, ESP_VISION_BOARD_ARCH, esp_get_idf_version(),
                       ev_control_bool_str(session.vm_ready));
    }
    if ((len <= 0) || ((size_t)len >= capacity)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t ev_control_send_hello_discovery(uint32_t seq)
{
    char payload[768];
    esp_err_t ret = ev_control_build_hello_discovery_payload(payload, sizeof(payload));
    if (ret == ESP_ERR_INVALID_STATE) {
        return ev_control_send_error("hello", seq, "INTERNAL_ERROR", "session state unavailable");
    }
    if (ret != ESP_OK) {
        return ret;
    }
    return ev_control_send_rpc_with_seq("rsp", "hello", payload, seq);
}

static esp_err_t ev_control_send_hello_acquire(uint32_t seq,
                                               const char *request,
                                               size_t request_len)
{
    ev_control_json_hello_t hello;
    if (!ev_control_json_parse_hello(request, request_len, &hello)) {
        return ev_control_send_error("hello", seq, "INVALID_ARGUMENT", "invalid hello JSON");
    }
    if (!hello.acquire) {
        return ev_control_send_hello_discovery(seq);
    }

    if (!hello.has_ev_mux_version) {
        return ev_control_send_error("hello", seq, "INVALID_ARGUMENT", "evMuxVersion must be an integer");
    }
    if (hello.ev_mux_version != 4) {
        return ev_control_send_error("hello", seq, "VERSION_MISMATCH", "acquire requires evMuxVersion 4");
    }
    if (!hello.has_client_nonce) {
        return ev_control_send_error("hello", seq, "INVALID_ARGUMENT", "invalid clientNonce");
    }
    ev_session_snapshot_t session;
    ev_session_result_t result = ev_session_acquire(&s_response_link, hello.client_nonce,
                                                    ev_control_now_ms(), &session);
    if (result == EV_SESSION_RESULT_BUSY) {
        // The release ACK can reach the host just before its TX completion
        // retires ownership. Keep that same link open for a bounded retry.
        bool releasing_owner = (session.state == EV_SESSION_RELEASING) &&
                               (session.owner_link.transport_id == s_response_link.transport_id) &&
                               (session.owner_link.slot == s_response_link.slot) &&
                               (session.owner_link.generation == s_response_link.generation);
        char payload[320];
        int len = snprintf(payload, sizeof(payload),
                           "{\"ok\":false,\"error\":{\"code\":\"%s\","
                           "\"message\":\"%s\"},"
                           "\"owner\":\"%s\",\"leaseRemainingMs\":%" PRIu32 "}",
                           releasing_owner ? "SESSION_RELEASING" : "SESSION_BUSY",
                           releasing_owner ? "previous session release is completing" : "another link owns the device",
                           ev_transport_name(session.owner_link.transport_id), session.lease_remaining_ms);
        if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((s_response_link.transport_id == EV_TRANSPORT_TCP) && !releasing_owner) {
            (void)ev_control_set_completion(EV_CONTROL_TX_DONE_CLOSE_LINK, "");
        }
        esp_err_t ret = ev_control_send_rpc_with_seq("rsp", "hello", payload, seq);
        if ((ret != ESP_OK) && (s_response_link.transport_id == EV_TRANSPORT_TCP) && !releasing_owner) {
            ev_transport_link_close(&s_response_link);
        }
        return ret;
    }
    if (result != EV_SESSION_RESULT_OK) {
        return ev_control_send_error("hello", seq,
                                     ev_session_result_code(result), "could not acquire session");
    }

    char payload[768];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"device\":\"esp-vision\","
                       "\"deviceId\":\"%s\",\"hostname\":\"%s\","
                       "\"firmware\":{\"id\":\"%s\",\"version\":\"%s\"},"
                       "\"board\":\"%s\",\"arch\":\"%s\",\"idf\":\"%s\","
                       "\"evMuxVersion\":4,\"vmReady\":%s,"
                       "\"session\":{\"id\":\"%s\",\"owner\":\"%s\","
                       "\"leaseMs\":%u,\"keepaliveMs\":%u,\"vmEpoch\":%" PRIu32 "},"
                       "\"routes\":{\"user\":\"%s\",\"debug\":\"%s\"}}",
                       esp_vision_device_id(), esp_vision_hostname(),
                       ESP_VISION_FIRMWARE_ID, ESP_VISION_FIRMWARE_VERSION,
                       ESP_VISION_BOARD_TYPE, ESP_VISION_BOARD_ARCH, esp_get_idf_version(),
                       ev_control_bool_str(session.vm_ready),
                       session.session_id, ev_transport_name(session.owner_link.transport_id),
                       (unsigned int)EV_SESSION_LEASE_MS, (unsigned int)EV_SESSION_KEEPALIVE_MS,
                       session.vm_epoch,
                       ev_transport_name(session.user_route), ev_transport_name(session.debug_route));
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        (void)ev_session_hello_complete(&s_response_link, session.session_id, false);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_response_session_id, session.session_id, sizeof(s_response_session_id));
    s_response_vm_epoch = session.vm_epoch;
    (void)ev_control_set_completion(EV_CONTROL_TX_DONE_HELLO, session.session_id);
    esp_err_t ret = ev_control_send_rpc_with_seq("rsp", "hello", payload, seq);
    if (ret != ESP_OK) {
        s_response_completion = NULL;
        s_response_completion_context_len = 0;
        (void)ev_session_hello_complete(&s_response_link, session.session_id, false);
    }
    return ret;
}

static esp_err_t ev_control_send_transport_state(uint32_t seq)
{
    ev_session_snapshot_t session;
    (void)ev_session_snapshot(&session);
    char payload[1024];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"defaultSink\":\"%s\",\"fallbackSink\":\"%s\","
                       "\"owner\":%s,\"sessionId\":\"%s\",\"vmEpoch\":%" PRIu32 ","
                       "\"user\":{\"route\":\"%s\",\"auto\":false,\"ready\":%s},"
                       "\"debug\":{\"route\":\"%s\",\"auto\":false,\"ready\":%s},"
                       "\"sinks\":{\"usj\":{\"state\":\"%s\",\"supported\":%s,\"ready\":%s},"
                       "\"usb\":{\"state\":\"%s\",\"supported\":%s,\"ready\":%s,\"present\":%s,\"connected\":%s},"
                       "\"uart\":{\"state\":\"%s\",\"supported\":%s,\"ready\":%s},"
                       "\"tcp\":{\"state\":\"%s\",\"supported\":%s,\"ready\":%s}}}",
                       ESP_VISION_TRANSPORT_DEFAULT_SINK,
                       ESP_VISION_TRANSPORT_FALLBACK_SINK,
                       session.has_owner ? "true" : "false",
                       session.has_owner ? session.session_id : "",
                       session.vm_epoch,
                       ev_channel_get(EV_STREAM_USER),
                       ev_control_bool_str(ev_channel_ready(EV_STREAM_USER)),
                       ev_channel_get(EV_STREAM_DEBUG),
                       ev_control_bool_str(ev_channel_ready(EV_STREAM_DEBUG)),
                       ev_channel_sink_state("usj"),
                       ev_control_bool_str(ev_channel_sink_supported("usj")),
                       ev_control_bool_str(ev_channel_sink_ready("usj")),
                       ev_channel_sink_state("usb"),
                       ev_control_bool_str(ev_channel_sink_supported("usb")),
                       ev_control_bool_str(ev_channel_sink_ready("usb")),
                       ev_control_json_nullable_bool(true, ev_channel_sink_present("usb")),
                       ev_control_json_nullable_bool(true, ev_channel_sink_ready("usb")),
                       ev_channel_sink_state("uart"),
                       ev_control_bool_str(ev_channel_sink_supported("uart")),
                       ev_control_bool_str(ev_channel_sink_ready("uart")),
                       ev_channel_sink_state("tcp"),
                       ev_control_bool_str(ev_channel_sink_supported("tcp")),
                       ev_control_bool_str(ev_channel_sink_ready("tcp")));
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "transport.state", payload, seq);
}

static esp_err_t ev_control_send_route_get(uint32_t seq, const char *request)
{
    char stream_name[16] = { 0 };
    if (!ev_control_json_get_string(request, "stream", stream_name, sizeof(stream_name))) {
        return ev_control_send_error("route.get", seq, "INVALID_ARGUMENT", "missing stream");
    }
    ev_stream_t stream = ev_control_stream_from_name(stream_name);
    if (stream == EV_STREAM_MAX) {
        return ev_control_send_error("route.get", seq, "INVALID_STREAM", "unknown stream");
    }

    char payload[192];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"stream\":\"%s\",\"route\":\"%s\",\"auto\":%s,\"ready\":%s}",
                       stream_name,
                       ev_channel_get(stream),
                       "false",
                       ev_control_bool_str(ev_channel_ready(stream)));
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "route.get", payload, seq);
}

static esp_err_t ev_control_send_route_bind(uint32_t seq, const char *request)
{
    (void)request;
    return ev_control_send_error("route.bind", seq, "UNSUPPORTED_OPERATION", "use session.release and hello");
}

static esp_err_t ev_control_send_route_auto(uint32_t seq, const char *request)
{
    (void)request;
    return ev_control_send_error("route.auto", seq, "UNSUPPORTED_OPERATION", "use session.release and hello");
}

static esp_err_t ev_control_send_script_write(uint32_t seq, const char *request)
{
    char path[EV_CONTROL_PATH_PARSE_CAP];
    char mode[16] = "overwrite";
    char encoding[16] = "utf-8";
    char content_base64[EV_CONTROL_PAYLOAD_MAX];
    bool has_offset = ev_control_json_find_value(request, "offset") != NULL;
    bool has_total = ev_control_json_find_value(request, "totalBytes") != NULL;
    uint32_t offset = ev_control_json_get_u32(request, "offset", 0);
    uint32_t total_bytes = ev_control_json_get_u32(request, "totalBytes", 0);
    if (!ev_control_json_get_string(request, "path", path, sizeof(path)) ||
            !ev_control_json_get_string(request, "contentBase64", content_base64, sizeof(content_base64))) {
        return ev_control_send_error("script.write", seq, "INVALID_ARGUMENT", "missing path or contentBase64");
    }
    (void)ev_control_json_get_string(request, "mode", mode, sizeof(mode));
    (void)ev_control_json_get_string(request, "encoding", encoding, sizeof(encoding));
    if (ev_control_path_too_long(path)) {
        return ev_control_send_error("script.write", seq, "PATH_TOO_LONG", "script path too long");
    }
    if (!ev_control_path_allowed(path)) {
        return ev_control_send_error("script.write", seq, "INVALID_PATH", "path is not allowed");
    }
    if ((strcmp(mode, "overwrite") != 0) && (strcmp(mode, "create") != 0)) {
        return ev_control_send_error("script.write", seq, "INVALID_MODE", "unsupported mode");
    }
    if ((strcmp(encoding, "utf-8") != 0) && (strcmp(encoding, "binary") != 0)) {
        return ev_control_send_error("script.write", seq, "INVALID_ENCODING", "unsupported encoding");
    }
    if (has_offset != has_total) {
        return ev_control_send_error("script.write", seq, "INVALID_ARGUMENT", "offset and totalBytes must be provided together");
    }
    if (has_total && (offset > total_bytes)) {
        return ev_control_send_error("script.write", seq, "INVALID_ARGUMENT", "offset exceeds totalBytes");
    }
    if (strcmp(mode, "create") == 0) {
        bool exists = false;
        if (ev_control_vfs_exists(path, &exists) != ESP_OK) {
            return ev_control_send_error("script.write", seq, "VFS_FAILED", "failed to inspect target path");
        }
        if (exists) {
            return ev_control_send_error("script.write", seq, "FILE_EXISTS", "target already exists");
        }
    }

    size_t decoded_capacity = (strlen(content_base64) * 3 / 4) + 4;
    uint8_t *decoded = heap_caps_malloc(decoded_capacity, MALLOC_CAP_8BIT);
    if (decoded == NULL) {
        return ev_control_send_error("script.write", seq, "NO_MEM", "failed to allocate decode buffer");
    }

    size_t decoded_len = 0;
    esp_err_t ret = ev_control_base64_decode(content_base64, decoded, decoded_capacity, &decoded_len);
    if (ret != ESP_OK) {
        heap_caps_free(decoded);
        return ev_control_send_error("script.write", seq, "INVALID_BASE64", esp_err_to_name(ret));
    }

    char tmp_path[EV_CONTROL_PATH_TMP_CAP];
    int tmp_len = snprintf(tmp_path, sizeof(tmp_path), "%s%s", path, EV_CONTROL_PATH_TMP_SUFFIX);
    if ((tmp_len <= 0) || ((size_t)tmp_len >= sizeof(tmp_path))) {
        heap_caps_free(decoded);
        return ev_control_send_error("script.write", seq, "PATH_TOO_LONG", "temporary path too long");
    }

    bool complete = true;
    if (has_total) {
        if ((uint64_t)offset + (uint64_t)decoded_len > (uint64_t)total_bytes) {
            heap_caps_free(decoded);
            return ev_control_send_error("script.write", seq, "INVALID_ARGUMENT", "chunk exceeds totalBytes");
        }
        complete = ((uint64_t)offset + (uint64_t)decoded_len) == (uint64_t)total_bytes;
    }

    if (!has_total || offset == 0) {
        if (ev_control_vfs_remove_if_exists(tmp_path) != ESP_OK) {
            heap_caps_free(decoded);
            return ev_control_send_error("script.write", seq, "VFS_FAILED", "failed to prepare temporary file");
        }
        ret = ev_control_vfs_write_file(tmp_path, decoded, decoded_len);
    } else {
        bool tmp_exists = false;
        if ((ev_control_vfs_exists(tmp_path, &tmp_exists) != ESP_OK) || !tmp_exists) {
            heap_caps_free(decoded);
            return ev_control_send_error("script.write", seq, "INVALID_STATE", "missing temporary file for chunk");
        }
        ret = ev_control_vfs_append_file(tmp_path, decoded, decoded_len);
    }
    if (ret != ESP_OK) {
        heap_caps_free(decoded);
        (void)ev_control_vfs_remove_if_exists(tmp_path);
        return ev_control_send_error("script.write", seq, "WRITE_FAILED", "failed to write temporary file");
    }
    heap_caps_free(decoded);

    if (complete) {
        if (ev_control_vfs_rename(tmp_path, path) != ESP_OK) {
            (void)ev_control_vfs_remove_if_exists(tmp_path);
            return ev_control_send_error("script.write", seq, "RENAME_FAILED", "failed to publish file");
        }
    }

    bool run_after_write = ev_control_json_get_bool(request, "runAfterWrite", false);
    if (run_after_write && complete) {
        char repl_cmd[192];
        int repl_len = snprintf(repl_cmd, sizeof(repl_cmd), "execfile(%c%s%c)\r\n", '"', path, '"');
        if ((repl_len > 0) && ((size_t)repl_len < sizeof(repl_cmd))) {
            (void)ev_control_repl_rx_push((const uint8_t *)repl_cmd, (size_t)repl_len,
                                          &s_response_link, s_response_session_id,
                                          s_response_vm_epoch, s_response_input_generation,
                                          s_response_transition_id);
        }
    }

    // Keep enough room for the complete structured acknowledgement and 32-bit
    // counters after applying the staging-path length contract above.
    char payload[256];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"path\":\"%s\",\"bytes\":%u,\"offset\":%u,"
                       "\"totalBytes\":%u,\"complete\":%s,\"runAfterWrite\":%s}",
                       path,
                       (unsigned int)decoded_len,
                       (unsigned int)(has_total ? offset : 0),
                       (unsigned int)(has_total ? total_bytes : decoded_len),
                       ev_control_bool_str(complete),
                       ev_control_bool_str(run_after_write));
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "script.write", payload, seq);
}

static esp_err_t ev_control_send_script_run(uint32_t seq, const char *request)
{
    char path[EV_CONTROL_PATH_PARSE_CAP];
    if (!ev_control_json_get_string(request, "path", path, sizeof(path))) {
        return ev_control_send_error("script.run", seq, "INVALID_ARGUMENT", "missing path");
    }
    if (ev_control_path_too_long(path)) {
        return ev_control_send_error("script.run", seq, "PATH_TOO_LONG", "script path too long");
    }
    if (!ev_control_path_allowed(path)) {
        return ev_control_send_error("script.run", seq, "INVALID_PATH", "path is not allowed");
    }

    char trial_operation[EV_LAUNCHER_OPERATION_ID_LEN + 1] = { 0 };
    char trial_source_path[EV_LAUNCHER_SOURCE_PATH_MAX + 1] = { 0 };
    char trial_source_hash[EV_LAUNCHER_SOURCE_HASH_HEX_LEN + 1] = { 0 };
    const bool has_trial_operation = ev_control_json_get_string(request, "trial_operation_id",
                                                                trial_operation, sizeof(trial_operation));
    const bool has_trial_source_path = ev_control_json_get_string(request, "trial_source_path",
                                                                  trial_source_path, sizeof(trial_source_path));
    const bool has_trial_source_hash = ev_control_json_get_string(request, "trial_source_hash",
                                                                  trial_source_hash, sizeof(trial_source_hash));
    const bool trial_requested = has_trial_operation || has_trial_source_path || has_trial_source_hash;
    if (trial_requested && !(has_trial_operation && has_trial_source_path && has_trial_source_hash)) {
        return ev_control_send_error("script.run", seq, "INVALID_ARGUMENT", "incomplete trial identity");
    }
    if (trial_requested && !esp_vision_launcher_arm_trial(trial_operation, path,
                                                          trial_source_path, trial_source_hash)) {
        return ev_control_send_error("script.run", seq, "TRIAL_NOT_AUTHORIZED", "trial runner identity is invalid or already active");
    }

    bool exists = false;
    if (ev_control_vfs_exists(path, &exists) != ESP_OK) {
        if (trial_requested) {
            (void)esp_vision_launcher_cancel_trial_arm(trial_operation);
        }
        return ev_control_send_error("script.run", seq, "VFS_FAILED", "failed to inspect script path");
    }
    if (!exists) {
        if (trial_requested) {
            (void)esp_vision_launcher_cancel_trial_arm(trial_operation);
        }
        return ev_control_send_error("script.run", seq, "NOT_FOUND", "script not found");
    }

    char repl_cmd[192];
    int repl_len = snprintf(repl_cmd, sizeof(repl_cmd), "execfile(%c%s%c)\r\n", '"', path, '"');
    if ((repl_len <= 0) || ((size_t)repl_len >= sizeof(repl_cmd))) {
        if (trial_requested) {
            (void)esp_vision_launcher_cancel_trial_arm(trial_operation);
        }
        return ev_control_send_error("script.run", seq, "PATH_TOO_LONG", "script path too long");
    }
    if (!ev_control_repl_rx_push((const uint8_t *)repl_cmd, (size_t)repl_len,
                                 &s_response_link, s_response_session_id,
                                 s_response_vm_epoch, s_response_input_generation,
                                 s_response_transition_id)) {
        if (trial_requested) {
            (void)esp_vision_launcher_cancel_trial_arm(trial_operation);
        }
        return ev_control_send_error("script.run", seq, "REPL_BUSY", "failed to enqueue script");
    }

    // script.run accepts the same path contract as script.write.
    char payload[256];
    int len = snprintf(payload, sizeof(payload), "{\"ok\":true,\"path\":\"%s\",\"queued\":true}", path);
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "script.run", payload, seq);
}

static esp_err_t ev_control_send_debug_info_device(uint32_t seq)
{
    char payload[512];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"scope\":\"device\",\"device\":\"esp-vision\","
                       "\"deviceId\":\"%s\",\"hostname\":\"%s\","
                       "\"firmware\":{\"id\":\"%s\",\"version\":\"%s\"},"
                       "\"board\":\"%s\","
                       "\"arch\":\"%s\",\"idf\":\"%s\","
                       "\"protocol\":{\"evMuxVersion\":4}}",
                       esp_vision_device_id(), esp_vision_hostname(),
                       ESP_VISION_FIRMWARE_ID,
                       ESP_VISION_FIRMWARE_VERSION,
                       ESP_VISION_BOARD_TYPE,
                       ESP_VISION_BOARD_ARCH,
                       esp_get_idf_version());
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "debug.info", payload, seq);
}

static esp_err_t ev_control_send_debug_info_memory(uint32_t seq)
{
    char payload[384];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"scope\":\"memory\","
                       "\"heapFree\":%u,\"heapLargest\":%u,"
                       "\"psramFree\":%u,\"psramLargest\":%u}",
                       (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                       (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                       (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                       (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "debug.info", payload, seq);
}

static esp_err_t ev_control_send_debug_info_sensor(uint32_t seq)
{
    esp_vision_camera_status_t status;
    esp_err_t ret = esp_vision_camera_get_status(&status);
    if (ret != ESP_OK) {
        return ev_control_send_error("debug.info", seq, "SENSOR_FAILED", esp_err_to_name(ret));
    }

    char payload[512];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"scope\":\"sensor\",\"ready\":%s,\"id\":%u,"
                       "\"width\":%u,\"height\":%u,\"pixformat\":%u,"
                       "\"hmirror\":%s,\"vflip\":%s}",
                       ev_control_bool_str(status.ready),
                       (unsigned int)status.sensor_id,
                       (unsigned int)status.width,
                       (unsigned int)status.height,
                       (unsigned int)status.pixfmt,
                       ev_control_bool_str(status.hmirror),
                       ev_control_bool_str(status.vflip));
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "debug.info", payload, seq);
}

static esp_err_t ev_control_send_debug_info_fs_list(uint32_t seq, const char *request)
{
    char path[EV_CONTROL_PATH_PARSE_CAP] = "/";
    const char *path_value = ev_control_json_find_value(request, "path");
    if ((path_value != NULL) && !ev_control_json_get_string(request, "path", path, sizeof(path))) {
        return ev_control_send_error("debug.info", seq, "INVALID_ARGUMENT", "invalid filesystem path");
    }
    if (ev_control_path_too_long(path)) {
        return ev_control_send_error("debug.info", seq, "PATH_TOO_LONG", "filesystem path too long");
    }
    if (!ev_control_path_allowed(path)) {
        return ev_control_send_error("debug.info", seq, "INVALID_PATH", "path is not allowed");
    }

    char payload[1024];
    int pos = snprintf(payload, sizeof(payload), "{\"ok\":true,\"scope\":\"fs.list\",\"path\":\"%s\",\"entries\":[", path);
    if ((pos <= 0) || ((size_t)pos >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }

    bool first = true;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t path_obj = mp_obj_new_str_from_cstr(path);
        mp_obj_t iter = mp_vfs_ilistdir(1, &path_obj);
        mp_obj_t item;
        while ((item = mp_iternext(iter)) != MP_OBJ_STOP_ITERATION) {
            size_t item_len = 0;
            mp_obj_t *items = NULL;
            mp_obj_get_array(item, &item_len, &items);
            if ((item_len == 0) || (items == NULL)) {
                continue;
            }

            char escaped[160];
            ev_control_json_escape(escaped, sizeof(escaped), mp_obj_str_get_str(items[0]));
            int n = snprintf(payload + pos,
                             sizeof(payload) - (size_t)pos,
                             "%s\"%s\"",
                             first ? "" : ",",
                             escaped);
            if ((n <= 0) || ((size_t)n >= (sizeof(payload) - (size_t)pos))) {
                break;
            }
            pos += n;
            first = false;
        }
        nlr_pop();
    } else {
        return ev_control_send_error("debug.info", seq, "OPEN_DIR_FAILED", "failed to open directory");
    }

    if ((size_t)pos + 3 >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(payload + pos, sizeof(payload) - (size_t)pos, "]}");
    return ev_control_send_rpc_with_seq("rsp", "debug.info", payload, seq);
}

static esp_err_t ev_control_send_debug_info_fs_read(uint32_t seq, const char *request)
{
    char path[EV_CONTROL_PATH_PARSE_CAP];
    if (!ev_control_json_get_string(request, "path", path, sizeof(path))) {
        return ev_control_send_error("debug.info", seq, "INVALID_ARGUMENT", "missing path");
    }
    if (ev_control_path_too_long(path)) {
        return ev_control_send_error("debug.info", seq, "PATH_TOO_LONG", "filesystem path too long");
    }
    if (!ev_control_path_allowed(path)) {
        return ev_control_send_error("debug.info", seq, "INVALID_PATH", "path is not allowed");
    }

    uint8_t data[EV_CONTROL_FILE_READ_MAX];
    size_t read_len = 0;
    if (ev_control_vfs_read_file(path, data, sizeof(data), &read_len) != ESP_OK) {
        return ev_control_send_error("debug.info", seq, "OPEN_FAILED", "failed to open file");
    }

    char encoded[((EV_CONTROL_FILE_READ_MAX + 2) / 3) * 4 + 1];
    esp_err_t ret = ev_control_base64_encode(data, read_len, encoded, sizeof(encoded));
    if (ret != ESP_OK) {
        return ev_control_send_error("debug.info", seq, "ENCODE_FAILED", esp_err_to_name(ret));
    }

    char payload[768];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"scope\":\"fs.read\",\"path\":\"%s\","
                       "\"encoding\":\"base64\",\"contentBase64\":\"%s\",\"bytes\":%u}",
                       path,
                       encoded,
                       (unsigned int)read_len);
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "debug.info", payload, seq);
}

static esp_err_t ev_control_send_debug_info_transport_stats(uint32_t seq)
{
    char payload[640];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"scope\":\"transport.stats\","
                       "\"rx\":{\"usj\":%" PRIu32 ",\"usb\":%" PRIu32 ",\"uart\":%" PRIu32 ",\"tcp\":%" PRIu32 "},"
                       "\"ringFull\":{\"usj\":%" PRIu32 ",\"usb\":%" PRIu32 ",\"uart\":%" PRIu32 "},"
                       "\"frames\":{\"ok\":%" PRIu32 ",\"bad\":%" PRIu32 ",\"rejected\":%" PRIu32 "},"
                       "\"tcpTaskStackMinFreeBytes\":%" PRIu32 ","
                       "\"parserTimeouts\":%" PRIu32 ",\"staleGenerationDrops\":%" PRIu32 ","
                       "\"drops\":{\"replRx\":%" PRIu32 ",\"vmBusy\":%" PRIu32 ","
                       "\"txTimeout\":%" PRIu32 ",\"txDrop\":%" PRIu32 ","
                       "\"txQueueFull\":%" PRIu32 ",\"txStale\":%" PRIu32 "}}",
                       s_stats.rx_bytes[EV_TRANSPORT_USJ],
                       s_stats.rx_bytes[EV_TRANSPORT_USB],
                       s_stats.rx_bytes[EV_TRANSPORT_UART],
                       s_stats.rx_bytes[EV_TRANSPORT_TCP],
                       ev_transport_rx_ring_full[EV_CONTROL_INGRESS_USJ],
                       ev_transport_rx_ring_full[EV_CONTROL_INGRESS_USB],
                       ev_transport_rx_ring_full[EV_CONTROL_INGRESS_UART],
                       s_stats.frames_ok,
                       s_stats.frames_bad,
                       s_stats.frames_rejected,
                       ev_tcp_task_stack_min_free_bytes(),
                       s_stats.parser_timeouts,
                       s_stats.stale_generation_drops,
                       s_stats.repl_rx_dropped,
                       s_stats.vm_busy,
                       ev_mux_tx_timeout_count(),
                       ev_mux_tx_drop_count(),
                       ev_mux_tx_queue_full_count(),
                       ev_mux_tx_stale_count());
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "debug.info", payload, seq);
}

static esp_err_t ev_control_send_debug_info(uint32_t seq, const char *request)
{
    char scope[24] = "device";
    (void)ev_control_json_get_string(request, "scope", scope, sizeof(scope));
    if (strcmp(scope, "device") == 0) {
        return ev_control_send_debug_info_device(seq);
    }
    if (strcmp(scope, "memory") == 0) {
        return ev_control_send_debug_info_memory(seq);
    }
    if (strcmp(scope, "sensor") == 0) {
        return ev_control_send_debug_info_sensor(seq);
    }
    if (strcmp(scope, "fs.list") == 0) {
        return ev_control_send_debug_info_fs_list(seq, request);
    }
    if (strcmp(scope, "fs.read") == 0) {
        return ev_control_send_debug_info_fs_read(seq, request);
    }
    if (strcmp(scope, "transport.stats") == 0) {
        return ev_control_send_debug_info_transport_stats(seq);
    }
    return ev_control_send_error("debug.info", seq, "INVALID_SCOPE", "unsupported debug.info scope");
}

static esp_err_t ev_control_send_debug_capture_frame(uint32_t seq, const char *request)
{
    int quality = (int)ev_control_json_get_u32(request, "quality", ESP_VISION_JPEG_QUALITY_LOW);
    if ((quality <= 0) || (quality > 100)) {
        quality = ESP_VISION_JPEG_QUALITY_LOW;
    }
    if (!esp_vision_camera_is_ready()) {
        esp_err_t init_ret = esp_vision_camera_init();
        if (init_ret != ESP_OK) {
            return ev_control_send_error("debug.capture_frame", seq, "CAMERA_INIT_FAILED", esp_err_to_name(init_ret));
        }
    }

    size_t frame_size = esp_vision_camera_frame_size();
    uint8_t *pixels = NULL;
    esp_err_t ret = esp_vision_camera_get_snapshot_buffer(frame_size, &pixels);
    if (ret != ESP_OK) {
        return ev_control_send_error("debug.capture_frame", seq, "NO_MEM", "failed to allocate frame buffer");
    }

    image_t img;
    ret = esp_vision_camera_snapshot(&img, pixels, frame_size);
    if (ret != ESP_OK) {
        return ev_control_send_error("debug.capture_frame", seq, "CAPTURE_FAILED", esp_err_to_name(ret));
    }

    uint8_t *jpeg_buf = NULL;
    size_t jpeg_size = 0;
    ret = esp_vision_jpeg_encode(&img, quality, &jpeg_buf, &jpeg_size);
    if (ret != ESP_OK) {
        return ev_control_send_error("debug.capture_frame", seq, "JPEG_FAILED", esp_err_to_name(ret));
    }

    ret = ev_control_send_binary_rsp("debug.capture_frame", "image/jpeg", jpeg_buf, jpeg_size,
                                     seq, img.w, img.h);
    if (ret != ESP_OK) {
        return ev_control_send_error("debug.capture_frame", seq, "SEND_FAILED", esp_err_to_name(ret));
    }
    return ESP_OK;
}

static esp_err_t ev_control_send_debug_capture_screen(uint32_t seq, const char *request)
{
    int quality = (int)ev_control_json_get_u32(request, "quality", ESP_VISION_JPEG_QUALITY_LOW);
    if ((quality <= 0) || (quality > 100)) {
        return ev_control_send_error("debug.capture_screen", seq, "INVALID_ARGUMENT", "quality must be 1..100");
    }
    // All display writes and this borrowed framebuffer view run on the VM
    // task, so encoding cannot race a write/deinit and needs no second frame.
    image_t frame;
    esp_err_t ret = esp_vision_display_get_frame(&frame);
    if (ret != ESP_OK) {
        return ev_control_send_error("debug.capture_screen", seq,
                                     (ret == ESP_ERR_NOT_SUPPORTED) ? "NOT_SUPPORTED" : "DISPLAY_NOT_READY",
                                     esp_err_to_name(ret));
    }
    uint8_t *jpeg_buf = NULL;
    size_t jpeg_size = 0;
    ret = esp_vision_jpeg_encode(&frame, quality, &jpeg_buf, &jpeg_size);
    if (ret != ESP_OK) {
        return ev_control_send_error("debug.capture_screen", seq, "JPEG_FAILED", esp_err_to_name(ret));
    }
    ret = ev_control_send_binary_rsp("debug.capture_screen", "image/jpeg", jpeg_buf, jpeg_size,
                                     seq, frame.w, frame.h);
    if (ret != ESP_OK) {
        return ev_control_send_error("debug.capture_screen", seq, "SEND_FAILED", esp_err_to_name(ret));
    }
    return ESP_OK;
}

static esp_err_t ev_control_send_storage_writer_control(uint32_t seq,
                                                        const char *request,
                                                        const char *action)
{
    char volume_name[8] = "";
    if (!ev_control_json_get_string(request, "volume", volume_name, sizeof(volume_name))) {
        return ev_control_send_error("device.control", seq, "INVALID_ARGUMENT", "missing storage volume");
    }
    esp_vision_storage_volume_t volume;
    if (strcmp(volume_name, "flash") == 0) {
        volume = ESP_VISION_STORAGE_VOLUME_FLASH;
    } else if (strcmp(volume_name, "sd") == 0) {
        volume = ESP_VISION_STORAGE_VOLUME_SD;
    } else {
        return ev_control_send_error("device.control", seq, "INVALID_ARGUMENT", "unsupported storage volume");
    }

    char payload[512];
    if (strcmp(action, "storage.writer.status") == 0) {
        esp_vision_storage_writer_status_t status;
        esp_err_t result = esp_vision_msc_writer_status(volume, &status);
        if (result == ESP_ERR_NOT_SUPPORTED) {
            memset(&status, 0, sizeof(status));
            status.mode = ESP_VISION_STORAGE_WRITER_UNKNOWN;
            status.reason = "WRITER_API_UNSUPPORTED";
        } else if ((result != ESP_OK) && (result != ESP_ERR_INVALID_STATE)) {
            return ev_control_send_error("device.control", seq, "WRITER_STATUS_UNKNOWN", "could not read writer status");
        }
        const char *mode = status.mode == ESP_VISION_STORAGE_WRITER_AVAILABLE ? "available" :
                           status.mode == ESP_VISION_STORAGE_WRITER_HELD ? "held" :
                           status.mode == ESP_VISION_STORAGE_WRITER_HANDOFF_READY ? "handoff_ready" :
                           status.mode == ESP_VISION_STORAGE_WRITER_QUARANTINED ? "quarantined" : "unknown";
        const bool present = volume == ESP_VISION_STORAGE_VOLUME_FLASH
                             ? esp_vision_storage_flash_is_present()
                             : esp_vision_sdcard_is_present();
        const bool mounted = volume == ESP_VISION_STORAGE_VOLUME_FLASH
                             ? esp_vision_storage_flash_is_mounted()
                             : esp_vision_storage_sdcard_is_mounted();
        const char *reason = status.reason != NULL ? status.reason : "";
        char operation_json[35] = "null";
        char lease_json[35] = "null";
        char owner_json[35] = "null";
        char reason_json[64] = "null";
        if (status.operation_id[0]) {
            (void)snprintf(operation_json, sizeof(operation_json), "\"%s\"", status.operation_id);
        }
        if (status.lease_id[0]) {
            (void)snprintf(lease_json, sizeof(lease_json), "\"%s\"", status.lease_id);
        }
        if (status.session_id[0]) {
            (void)snprintf(owner_json, sizeof(owner_json), "\"%s\"", status.session_id);
        }
        if (reason[0]) {
            (void)snprintf(reason_json, sizeof(reason_json), "\"%s\"", reason);
        }
        int len = snprintf(payload, sizeof(payload),
                           "{\"ok\":true,\"action\":\"storage.writer.status\",\"volume\":\"%s\","
                           "\"status\":\"%s\",\"writable\":%s,\"present\":%s,\"mounted\":%s,\"operation_id\":%s,"
                           "\"lease_id\":%s,\"session_id\":%s,\"reason\":%s}",
                           volume_name, mode, ev_control_bool_str(status.writable),
                           ev_control_bool_str(present), ev_control_bool_str(mounted),
                           operation_json, lease_json, owner_json, reason_json);
        if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
            return ev_control_send_error("device.control", seq, "INTERNAL_ERROR", "could not encode writer status");
        }
        return ev_control_send_rpc_with_seq("rsp", "device.control", payload, seq);
    }

    char requested_operation[33] = "";
    if (!ev_control_json_get_string(request, "operationId", requested_operation, sizeof(requested_operation))) {
        return ev_control_send_error("device.control", seq, "INVALID_ARGUMENT", "missing operation identity");
    }
    if (s_response_session_id[0] == '\0') {
        return ev_control_send_error("device.control", seq, "SESSION_REQUIRED", "an active authenticated session is required");
    }
    if (strcmp(action, "storage.writer.acquire") == 0) {
        char lease_id[33] = { 0 };
        esp_err_t result = esp_vision_msc_writer_acquire(volume, requested_operation,
                                                         s_response_session_id, lease_id);
        if (result != ESP_OK) {
            return ev_control_send_error("device.control", seq,
                                         result == ESP_ERR_INVALID_ARG ? "INVALID_ARGUMENT" : "WRITER_BUSY",
                                         "storage writer lease could not be acquired");
        }
        int len = snprintf(payload, sizeof(payload),
                           "{\"ok\":true,\"action\":\"storage.writer.acquire\",\"volume\":\"%s\","
                           "\"operation_id\":\"%s\",\"lease_id\":\"%s\",\"session_id\":\"%s\",\"writable\":true}",
                           volume_name, requested_operation, lease_id, s_response_session_id);
        if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
            return ev_control_send_error("device.control", seq, "WRITER_RESULT_UNKNOWN", "lease acquired but response encoding failed");
        }
        return ev_control_send_rpc_with_seq("rsp", "device.control", payload, seq);
    }

    char requested_lease[33] = "";
    if (!ev_control_json_get_string(request, "leaseId", requested_lease, sizeof(requested_lease)) ||
            !ev_control_json_get_bool(request, "safeHandoff", false)) {
        return ev_control_send_error("device.control", seq, "INVALID_ARGUMENT", "a lease identity and explicit safe handoff are required");
    }
    esp_err_t result = esp_vision_msc_writer_release(volume, requested_operation,
                                                     requested_lease, s_response_session_id, true);
    if (result != ESP_OK) {
        return ev_control_send_error("device.control", seq,
                                     result == ESP_ERR_INVALID_ARG ? "INVALID_ARGUMENT" : "WRITER_HANDOFF_UNKNOWN",
                                     "storage writer handoff could not be confirmed");
    }
    int len = snprintf(payload, sizeof(payload),
                       "{\"ok\":true,\"action\":\"storage.writer.release\",\"volume\":\"%s\","
                       "\"operation_id\":\"%s\",\"lease_id\":\"%s\",\"session_id\":\"%s\","
                       "\"status\":\"handoff_ready\",\"writable\":false}",
                       volume_name, requested_operation, requested_lease, s_response_session_id);
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ev_control_send_error("device.control", seq, "WRITER_RESULT_UNKNOWN", "handoff completed but response encoding failed");
    }
    return ev_control_send_rpc_with_seq("rsp", "device.control", payload, seq);
}

static esp_err_t ev_control_send_device_control(uint32_t seq, const char *request)
{
    char action[24];
    if (!ev_control_json_get_string(request, "action", action, sizeof(action))) {
        return ev_control_send_error("device.control", seq, "INVALID_ARGUMENT", "missing action");
    }
    char network_response[1024];
    bool response_barrier = false;
    char operation_id[33] = { 0 };
    if (esp_vision_network_rpc_handle(request, strlen(request), network_response,
                                      sizeof(network_response), &response_barrier, operation_id)) {
        if (response_barrier && !ev_control_set_completion(EV_CONTROL_TX_DONE_NETWORK, operation_id)) {
            // Let the service's bounded fallback start the operation. Releasing
            // the barrier here would run configuration before an accepted
            // response was actually delivered.
            return ev_control_send_error("device.control", seq, "INTERNAL_ERROR", "could not queue network response");
        }
        esp_err_t ret = ev_control_send_rpc_with_seq("rsp", "device.control", network_response, seq);
        if ((ret != ESP_OK) && response_barrier) {
            s_response_completion = NULL;
            s_response_completion_context_len = 0;
        }
        return ret;
    }
    if ((strcmp(action, "storage.writer.status") == 0) ||
            (strcmp(action, "storage.writer.acquire") == 0) ||
            (strcmp(action, "storage.writer.release") == 0)) {
        return ev_control_send_storage_writer_control(seq, request, action);
    }
    if (strcmp(action, "launcher.boot.status") == 0) {
        ev_launcher_state_snapshot_t state;
        if (!esp_vision_launcher_state_snapshot(&state) || state.boot_id[0] == '\0' || !state.device_id_valid) {
            return ev_control_send_error("device.control", seq, "BOOT_STATUS_UNKNOWN", "launcher boot state is unavailable");
        }
        char payload[1024];
        int len;
        if (state.selection_valid) {
            len = snprintf(payload, sizeof(payload),
                           "{\"ok\":true,\"action\":\"launcher.boot.status\",\"device_id\":\"%s\",\"boot_id\":\"%s\","
                           "\"vm_epoch\":%" PRIu32 ",\"selection\":{\"generation\":%" PRIu64 ","
                           "\"source_hash\":\"%s\",\"source_path\":\"%s\"},\"execution_outcome\":\"%s\"}",
                           state.device_id, state.boot_id, state.vm_epoch, state.generation, state.source_hash,
                           state.source_path, esp_vision_launcher_outcome_name(state.execution_outcome));
        } else {
            len = snprintf(payload, sizeof(payload),
                           "{\"ok\":true,\"action\":\"launcher.boot.status\",\"device_id\":\"%s\",\"boot_id\":\"%s\","
                           "\"vm_epoch\":%" PRIu32 ",\"selection\":null,\"execution_outcome\":\"%s\"}",
                           state.device_id, state.boot_id, state.vm_epoch,
                           esp_vision_launcher_outcome_name(state.execution_outcome));
        }
        if (len <= 0 || (size_t)len >= sizeof(payload)) {
            return ev_control_send_error("device.control", seq, "BOOT_STATUS_UNKNOWN", "launcher boot state could not be encoded");
        }
        return ev_control_send_rpc_with_seq("rsp", "device.control", payload, seq);
    }
    if (strcmp(action, "launcher.trial.status") == 0) {
        char operation_id[EV_LAUNCHER_OPERATION_ID_LEN + 1] = { 0 };
        if (!ev_control_json_get_string(request, "operation_id", operation_id, sizeof(operation_id))) {
            return ev_control_send_error("device.control", seq, "INVALID_ARGUMENT", "missing trial operation identity");
        }
        if (!esp_vision_launcher_valid_operation_id(operation_id)) {
            return ev_control_send_error("device.control", seq, "INVALID_ARGUMENT", "invalid trial operation identity");
        }
        ev_launcher_state_snapshot_t state;
        char payload[1200];
        int len;
        if (esp_vision_launcher_trial_snapshot(operation_id, &state)) {
            len = snprintf(payload, sizeof(payload),
                           "{\"ok\":true,\"action\":\"launcher.trial.status\",\"operation_id\":\"%s\","
                           "\"status\":\"%s\",\"vm_epoch\":%" PRIu32 ",\"source_hash\":\"%s\","
                           "\"source_path\":\"%s\",\"execution_outcome\":\"%s\"}",
                           state.operation_id,
                           esp_vision_launcher_trial_status_name(state.trial_status), state.trial_vm_epoch,
                           state.trial_source_hash, state.trial_source_path,
                           esp_vision_launcher_outcome_name(state.trial_outcome));
        } else {
            len = snprintf(payload, sizeof(payload),
                           "{\"ok\":true,\"action\":\"launcher.trial.status\",\"operation_id\":\"%s\","
                           "\"status\":\"not_started\",\"vm_epoch\":%" PRIu32 ",\"source_hash\":null,"
                           "\"source_path\":null,\"execution_outcome\":\"unknown\"}",
                           operation_id, ev_session_vm_epoch());
        }
        if (len <= 0 || (size_t)len >= sizeof(payload)) {
            return ev_control_send_error("device.control", seq, "TRIAL_STATUS_UNKNOWN", "trial state could not be encoded");
        }
        return ev_control_send_rpc_with_seq("rsp", "device.control", payload, seq);
    }
    if (strcmp(action, "launcher.trial.cancel") == 0) {
        char operation_id[EV_LAUNCHER_OPERATION_ID_LEN + 1] = { 0 };
        if (!ev_control_json_get_string(request, "operation_id", operation_id, sizeof(operation_id)) ||
                !esp_vision_launcher_valid_operation_id(operation_id)) {
            return ev_control_send_error("device.control", seq, "INVALID_ARGUMENT", "invalid trial operation identity");
        }
        if (!esp_vision_launcher_cancel_trial_arm(operation_id)) {
            return ev_control_send_error("device.control", seq, "TRIAL_NOT_ARMED", "the matching trial is no longer armed");
        }
        char payload[192];
        int len = snprintf(payload, sizeof(payload),
                           "{\"ok\":true,\"action\":\"launcher.trial.cancel\",\"operation_id\":\"%s\",\"cancelled\":true}",
                           operation_id);
        if (len <= 0 || (size_t)len >= sizeof(payload)) {
            return ev_control_send_error("device.control", seq, "TRIAL_CANCEL_UNKNOWN", "trial cancellation could not be encoded");
        }
        return ev_control_send_rpc_with_seq("rsp", "device.control", payload, seq);
    }
    if (strcmp(action, "status") == 0) {
        char payload[512];
        int len = snprintf(payload,
                           sizeof(payload),
                           "{\"ok\":true,\"action\":\"status\",\"uptimeMs\":%" PRIu32 ","
                           "\"user\":{\"route\":\"%s\",\"auto\":false,\"ready\":%s},"
                           "\"debug\":{\"route\":\"%s\",\"auto\":false,\"ready\":%s},"
                           "\"heapFree\":%u}",
                           ev_control_now_ms(),
                           ev_channel_get(EV_STREAM_USER),
                           ev_control_bool_str(ev_channel_ready(EV_STREAM_USER)),
                           ev_channel_get(EV_STREAM_DEBUG),
                           ev_control_bool_str(ev_channel_ready(EV_STREAM_DEBUG)),
                           (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ev_control_send_rpc_with_seq("rsp", "device.control", payload, seq);
    }
    if (strcmp(action, "stop") == 0) {
        ev_control_deliver_ctrl_c(&s_response_link, s_response_session_id,
                                  s_response_vm_epoch, s_response_input_generation,
                                  s_response_transition_id);
        return ev_control_send_rpc_with_seq("rsp", "device.control", "{\"ok\":true,\"action\":\"stop\"}", seq);
    }
    if (strcmp(action, "reset") == 0) {
        ev_mux_ticket_t ticket = 0;
        s_response_ticket_out = &ticket;
        esp_err_t ret = ev_control_send_rpc_with_seq("rsp", "device.control", "{\"ok\":true,\"action\":\"reset\"}", seq);
        s_response_ticket_out = NULL;
        if ((ret == ESP_OK) && (ticket != 0)) {
            esp_err_t tx_result = ESP_FAIL;
            (void)ev_mux_wait(ticket, 1000, &tx_result);
            ev_mux_ticket_forget(ticket);
        }
        esp_restart();
        return ret;
    }
    return ev_control_send_error("device.control", seq, "INVALID_ACTION", "unsupported action");
}

static void ev_control_handle_user_rpc(uint32_t seq, const char *method, const char *payload)
{
    if (strcmp(method, "session.keepalive") == 0) {
        ev_session_snapshot_t session;
        ev_session_result_t result = ev_session_keepalive(&s_response_link,
                                                          s_response_session_id,
                                                          ev_control_now_ms(),
                                                          &session);
        if (result != EV_SESSION_RESULT_OK) {
            (void)ev_control_send_error("session.keepalive", seq,
                                        ev_session_result_code(result), "session is not active");
        } else {
            char response[192];
            // This is a transport-scoped reply, so report the readiness and
            // epoch from the keepalive snapshot itself. A VM reset may advance
            // the epoch while this request is being handled.
            s_response_vm_epoch = session.vm_epoch;
            int len = snprintf(response, sizeof(response),
                               "{\"ok\":true,\"action\":\"session.keepalive\","
                               "\"sessionId\":\"%s\",\"leaseMs\":%u,\"vmEpoch\":%" PRIu32 ","
                               "\"vmReady\":%s}",
                               session.session_id, (unsigned int)EV_SESSION_LEASE_MS,
                               session.vm_epoch, ev_control_bool_str(session.vm_ready));
            if ((len > 0) && ((size_t)len < sizeof(response))) {
                (void)ev_control_send_rpc_with_seq("rsp", "session.keepalive", response, seq);
            }
        }
    } else if (strcmp(method, "session.release") == 0) {
        ev_session_snapshot_t session;
        ev_session_result_t result = ev_session_release_prepare(&s_response_link,
                                                                s_response_session_id,
                                                                &session);
        if (result != EV_SESSION_RESULT_OK) {
            (void)ev_control_send_error("session.release", seq,
                                        ev_session_result_code(result), "session is not active");
        } else {
            if (!ev_control_set_completion(EV_CONTROL_TX_DONE_RELEASE, session.session_id)) {
                (void)ev_session_release_finish(&s_response_link, session.session_id,
                                                EV_SESSION_CHANGE_RELEASED);
                (void)ev_control_send_error("session.release", seq,
                                            "INTERNAL_ERROR", "could not queue release response");
                return;
            }
            esp_err_t ret = ev_control_send_rpc_with_seq("rsp", "session.release",
                                                         "{\"ok\":true,\"accepted\":true}", seq);
            if (ret != ESP_OK) {
                s_response_completion = NULL;
                s_response_completion_context_len = 0;
                (void)ev_session_release_finish(&s_response_link, session.session_id,
                                                EV_SESSION_CHANGE_RELEASED);
            }
        }
    } else if (strcmp(method, "script.write") == 0) {
        (void)ev_control_send_script_write(seq, payload);
    } else if (strcmp(method, "script.run") == 0) {
        (void)ev_control_send_script_run(seq, payload);
    } else if (strcmp(method, "device.control") == 0) {
        (void)ev_control_send_device_control(seq, payload);
    } else {
        (void)ev_control_send_error(method, seq, "UNKNOWN_METHOD", "unsupported user RPC method");
    }
}

static void ev_control_handle_debug_rpc(uint32_t seq, const char *method, const char *payload)
{
    if (strcmp(method, "transport.state") == 0) {
        (void)ev_control_send_transport_state(seq);
    } else if (strcmp(method, "route.get") == 0) {
        (void)ev_control_send_route_get(seq, payload);
    } else if (strcmp(method, "route.bind") == 0) {
        (void)ev_control_send_route_bind(seq, payload);
    } else if (strcmp(method, "route.auto") == 0) {
        (void)ev_control_send_route_auto(seq, payload);
    } else if (strcmp(method, "debug.info") == 0) {
        (void)ev_control_send_debug_info(seq, payload);
    } else if (strcmp(method, "debug.capture_frame") == 0) {
        (void)ev_control_send_debug_capture_frame(seq, payload);
    } else if (strcmp(method, "debug.capture_screen") == 0) {
        (void)ev_control_send_debug_capture_screen(seq, payload);
    } else {
        (void)ev_control_send_error(method, seq, "UNKNOWN_METHOD", "unsupported debug RPC method");
    }
}

static bool ev_control_is_bootstrap_method(const char *channel, const char *method)
{
    return (strcmp(channel, "user.rpc") == 0) &&
           ((strcmp(method, "hello") == 0) || (strcmp(method, "capabilities") == 0));
}

static void ev_control_log_rejected_frame(const ev_link_ref_t *link,
                                          const char *channel,
                                          const char *method)
{
    (void)link;
    (void)channel;
    (void)method;
    // Do not emit diagnostics over a link whose parser or TX path just failed.
    // Counters are the bounded, non-recursive diagnostic path.
    ++s_stats.frames_rejected;
}

static bool ev_control_frame_authorized(const ev_link_ref_t *link,
                                        const char *channel,
                                        const char *method,
                                        const ev_control_json_metadata_t *metadata,
                                        const char *payload,
                                        size_t payload_len,
                                        ev_session_snapshot_t *out_session,
                                        ev_session_result_t *out_result)
{
    if (ev_control_is_bootstrap_method(channel, method)) {
        *out_result = EV_SESSION_RESULT_OK;
        return true;
    }
    bool is_user_or_debug_rpc = (strcmp(channel, "user.rpc") == 0) ||
                                (strcmp(channel, "debug.rpc") == 0);
    bool is_repl_stdin = strcmp(channel, "repl.stdin") == 0;
    bool is_repl_signal = strcmp(channel, "repl.signal") == 0;
    if (!is_user_or_debug_rpc && !is_repl_stdin && !is_repl_signal) {
        return false;
    }
    if (is_repl_stdin && (payload_len > EV_CONTROL_REPL_INPUT_MAX)) {
        *out_result = EV_SESSION_RESULT_INVALID_ARGUMENT;
        return true;
    }
    if (is_repl_signal && ((payload_len != 1) || (payload[0] != 0x03))) {
        // A rejected signal is not valid owner activity and must not refresh
        // the session lease through the later authorization path.
        *out_result = EV_SESSION_RESULT_INVALID_ARGUMENT;
        return true;
    }

    bool vm_related = is_repl_stdin || is_repl_signal ||
                      (is_user_or_debug_rpc && strcmp(method, "file.write") == 0) ||
                      (is_user_or_debug_rpc && ev_control_rpc_needs_vm(
                           (strcmp(channel, "debug.rpc") == 0) ? EV_RPC_DEBUG : EV_RPC_USER,
                           method, payload));
    *out_result = ev_session_authorize(link,
                                       metadata->has_session_id ? metadata->session_id : NULL,
                                       vm_related,
                                       metadata->has_vm_epoch,
                                       metadata->vm_epoch,
                                       ev_control_now_ms(),
                                       out_session);
    return true;
}

// RPC methods that touch MicroPython objects, the VFS or the camera must run
// on the VM task; everything else is answered directly on the transport task.
static bool ev_control_rpc_needs_vm(ev_control_rpc_domain_t domain, const char *method, const char *payload)
{
    if (domain == EV_RPC_USER) {
        return (strcmp(method, "script.write") == 0) ||
               (strcmp(method, "script.run") == 0);
    }
    if ((strcmp(method, "debug.capture_frame") == 0) ||
            (strcmp(method, "debug.capture_screen") == 0)) {
        return true;
    }
    if (strcmp(method, "debug.info") == 0) {
        // transport.stats is pure C and must stay answerable while the VM
        // is busy; all other scopes involve MicroPython state or the camera.
        char scope[24] = "";
        (void)ev_control_json_get_string(payload, "scope", scope, sizeof(scope));
        return strcmp(scope, "transport.stats") != 0;
    }
    return false;
}

// Deliver Ctrl-C semantics exactly like a physical byte: when the VM is
// executing code with interrupts armed (parse_compile_execute arms
// mp_interrupt_char), schedule the KeyboardInterrupt; otherwise feed Ctrl-C
// as an input character (REPL editing key). Scheduling the interrupt outside
// the armed window would raise it where no NLR handler exists and trip
// nlr_jump_fail -> esp_restart().
static void ev_control_deliver_ctrl_c(const ev_link_ref_t *link,
                                      const char *session_id,
                                      uint32_t vm_epoch,
                                      uint32_t input_generation,
                                      uint32_t transition_id)
{
    ev_session_snapshot_t current;
    if ((ev_session_authorize(link, session_id, true, true, vm_epoch,
                              ev_control_now_ms(), &current) != EV_SESSION_RESULT_OK) ||
            (current.input_generation != input_generation) ||
            (current.transition_id != transition_id) ||
            (ev_session_input_generation() != input_generation)) {
        return;
    }
    if (mp_interrupt_char == CHAR_CTRL_C) {
        mp_sched_keyboard_interrupt();
    } else {
        (void)ev_control_repl_rx_push((const uint8_t *)"\x03", 1,
                                      link, session_id, vm_epoch,
                                      input_generation, transition_id);
    }
    mp_hal_wake_main_task();
}

static void ev_control_response_context_clear(void)
{
    s_response_rpc_domain = EV_RPC_USER;
    memset(&s_response_link, 0, sizeof(s_response_link));
    s_has_response_link = false;
    memset(s_response_session_id, 0, sizeof(s_response_session_id));
    s_response_vm_epoch = 0;
    s_response_tx_vm_epoch = 0;
    s_response_input_generation = 0;
    s_response_transition_id = 0;
    s_response_completion = NULL;
    s_response_completion_context_len = 0;
    s_response_ticket_out = NULL;
}

static bool ev_control_link_equal(const ev_link_ref_t *a, const ev_link_ref_t *b)
{
    return (a != NULL) && (b != NULL) && (a->transport_id == b->transport_id) &&
           (a->slot == b->slot) && (a->generation == b->generation);
}

static uint8_t *ev_control_frame_payload(ev_control_parser_t *parser)
{
    return parser->file_payload != NULL ? parser->file_payload : parser->payload;
}

static void ev_control_handle_file_write(uint32_t seq, const ev_control_json_metadata_t *metadata,
                                         const ev_session_snapshot_t *session,
                                         const uint8_t *data, size_t length)
{
    const char *error = NULL;
    uint32_t received = 0;
    char response[160];
    if (strcmp(metadata->encoding, "binary") == 0) {
        error = ev_file_transfer_write(session, data, length, &received);
        (void)snprintf(response, sizeof(response), "{\"ok\":true,\"writtenBytes\":%" PRIu32 "}", received);
    } else {
        char action[12] = "", path[EV_CONTROL_PATH_PARSE_CAP] = "", mode[12] = "overwrite";
        char sha256[65] = "", upload_id[33] = "";
        ev_control_json_field_t fields[] = {
            ev_control_json_string_field("action", true, action, sizeof(action)),
            ev_control_json_string_field("path", false, path, sizeof(path)),
            ev_control_json_string_field("mode", false, mode, sizeof(mode)),
            ev_control_json_uint32_field("totalBytes", false),
            ev_control_json_string_field("sha256", false, sha256, sizeof(sha256)),
            ev_control_json_string_field("uploadId", false, upload_id, sizeof(upload_id)),
        };
        if (!ev_control_json_parse_object((const char *)data, length, fields, MP_ARRAY_SIZE(fields))) {
            error = "INVALID_ARGUMENT";
        } else if (strcmp(action, "begin") == 0) {
            if (!fields[1].present || !fields[3].present || !fields[4].present || !fields[5].present ||
                    !ev_control_path_allowed(path) || (strcmp(mode, "create") != 0 && strcmp(mode, "overwrite") != 0)) {
                error = "INVALID_ARGUMENT";
            } else {
                error = ev_file_transfer_begin(session, path, strcmp(mode, "overwrite") == 0,
                                               fields[3].value.uint32, sha256, upload_id);
                (void)snprintf(response, sizeof(response),
                               "{\"ok\":true,\"uploadId\":\"%s\",\"chunkBytes\":%u}", upload_id, EV_FILE_CHUNK_BYTES);
            }
        } else if ((strcmp(action, "commit") == 0 || strcmp(action, "abort") == 0) && fields[5].present) {
            error = ev_file_transfer_end(session, upload_id, strcmp(action, "commit") == 0, &received);
            (void)snprintf(response, sizeof(response),
                           "{\"ok\":true,\"bytes\":%" PRIu32 ",\"complete\":%s}",
                           received, ev_control_bool_str(strcmp(action, "commit") == 0));
        } else {
            error = "INVALID_ARGUMENT";
        }
    }
    if (error != NULL) {
        (void)ev_control_send_error("file.write", seq, error, "binary file transfer failed");
    } else {
        (void)ev_control_send_rpc_with_seq("rsp", "file.write", response, seq);
    }
}

static void ev_control_handle_frame(ev_control_parser_t *parser, const ev_link_ref_t *link)
{
    uint8_t *payload = ev_control_frame_payload(parser);
    payload[parser->payload_len] = '\0';
    ev_control_json_metadata_t metadata;
    if (!ev_control_json_parse_metadata(parser->metadata, parser->metadata_len, &metadata)) {
        ev_control_log_rejected_frame(link, "<invalid>", "");
        return;
    }
    const char *channel = metadata.channel;
    const char *type = metadata.type;
    const char *method = metadata.method;
    uint32_t seq = metadata.seq;
    bool is_rpc = (strcmp(channel, "user.rpc") == 0) || (strcmp(channel, "debug.rpc") == 0);
    if (strcmp(type, "req") != 0) {
        ev_control_log_rejected_frame(link, channel, method);
        return;
    }
    bool file_write = strcmp(channel, "user.rpc") == 0 && strcmp(method, "file.write") == 0;
    bool binary_file = file_write && strcmp(metadata.encoding, "binary") == 0;
    if ((strcmp(metadata.encoding, "binary") == 0 && !binary_file) ||
            (is_rpc && !binary_file && !ev_control_json_rpc_payload_valid(payload, parser->payload_len))) {
        ev_control_log_rejected_frame(link, channel, method);
        return;
    }

    ev_control_rpc_domain_t domain = (strcmp(channel, "debug.rpc") == 0) ? EV_RPC_DEBUG : EV_RPC_USER;
    s_response_link = *link;
    s_has_response_link = true;
    s_response_rpc_domain = domain;
    s_response_tx_vm_epoch = 0;
    s_response_input_generation = 0;
    s_response_transition_id = 0;

    if (ev_control_is_bootstrap_method(channel, method)) {
        s_response_session_id[0] = '\0';
        s_response_vm_epoch = 0;
        if (strcmp(method, "hello") == 0) {
            (void)ev_control_send_hello_acquire(seq, (const char *)payload,
                                                parser->payload_len);
        } else {
            (void)ev_control_send_capabilities(seq);
        }
        ev_control_response_context_clear();
        return;
    }

    ev_session_snapshot_t session;
    ev_session_result_t auth_result = EV_SESSION_RESULT_SESSION_REQUIRED;
    if (!ev_control_frame_authorized(link, channel, method, &metadata,
                                     (const char *)payload, parser->payload_len,
                                     &session, &auth_result)) {
        ev_control_log_rejected_frame(link, channel, method);
        ev_control_response_context_clear();
        return;
    }
    if (auth_result != EV_SESSION_RESULT_OK) {
        if (((auth_result == EV_SESSION_RESULT_VM_EPOCH_MISMATCH) ||
                (auth_result == EV_SESSION_RESULT_VM_BUSY)) && session.has_owner &&
                ev_control_link_equal(link, &session.owner_link) && metadata.has_session_id &&
                (strcmp(metadata.session_id, session.session_id) == 0)) {
            memcpy(s_response_session_id, session.session_id, sizeof(s_response_session_id));
            s_response_vm_epoch = session.vm_epoch;
        }
        if (is_rpc) {
            (void)ev_control_send_error(method, seq, ev_session_result_code(auth_result),
                                        (auth_result == EV_SESSION_RESULT_VM_BUSY) ?
                                        "interpreter is restarting" : "request is not authorized");
        } else {
            ++s_stats.frames_rejected;
        }
        ev_control_response_context_clear();
        return;
    }

    memcpy(s_response_session_id, session.session_id, sizeof(s_response_session_id));
    s_response_vm_epoch = session.vm_epoch;
    s_response_input_generation = session.input_generation;
    s_response_transition_id = session.transition_id;
    if (file_write || (is_rpc && ev_control_rpc_needs_vm(domain, method, (const char *)payload))) {
        s_response_tx_vm_epoch = session.vm_epoch;
    }
    if (file_write) {
        ev_control_handle_file_write(seq, &metadata, &session, payload, parser->payload_len);
        ev_control_response_context_clear();
        return;
    }
    if (strcmp(channel, "repl.stdin") == 0) {
        if (!ev_control_repl_rx_push(payload, parser->payload_len,
                                     link, session.session_id, session.vm_epoch,
                                     session.input_generation, session.transition_id)) {
            ++s_stats.repl_rx_dropped;
        }
        mp_hal_wake_main_task();
        ev_control_response_context_clear();
        return;
    }
    if (strcmp(channel, "repl.signal") == 0) {
        ev_control_deliver_ctrl_c(link, session.session_id, session.vm_epoch,
                                  session.input_generation, session.transition_id);
        ev_control_response_context_clear();
        return;
    }

    if (is_rpc) {
        bool needs_vm = ev_control_rpc_needs_vm(domain, method, (const char *)payload);
        if (needs_vm) {
            if (!ev_control_vm_queue_push(domain, link, session.session_id, method, seq,
                                          session.vm_epoch,
                                          session.input_generation, session.transition_id,
                                          payload, parser->payload_len)) {
                ++s_stats.vm_busy;
                (void)ev_control_send_error(method, seq, "VM_BUSY", "interpreter busy");
            } else {
                mp_hal_wake_main_task();
            }
        } else if (domain == EV_RPC_DEBUG) {
            ev_control_handle_debug_rpc(seq, method, (const char *)payload);
        } else {
            ev_control_handle_user_rpc(seq, method, (const char *)payload);
        }
    }
    ev_control_response_context_clear();
}

static void ev_control_parser_reset_keep_link(ev_control_parser_t *parser)
{
    bool link_bound = parser->link_bound;
    ev_link_ref_t link = parser->link;
    ev_control_parser_reset(parser);
    parser->link_bound = link_bound;
    parser->link = link;
}

static void ev_control_parser_begin(ev_control_parser_t *parser,
                                    const ev_link_ref_t *link,
                                    uint32_t now_ms)
{
    ev_control_parser_reset(parser);
    parser->link_bound = true;
    parser->link = *link;
    parser->state = EV_CONTROL_PARSE_HEADER;
    parser->frame_start_ms = now_ms;
    parser->last_progress_ms = now_ms;
}

static bool ev_control_parser_expire(ev_control_parser_t *parser, uint32_t now_ms)
{
    if (parser->state == EV_CONTROL_PARSE_SOF) {
        return false;
    }
    if (((uint32_t)(now_ms - parser->last_progress_ms) < EV_CONTROL_FRAME_IDLE_TIMEOUT_MS) &&
            ((uint32_t)(now_ms - parser->frame_start_ms) < EV_CONTROL_FRAME_TOTAL_TIMEOUT_MS)) {
        return false;
    }
    ++s_stats.parser_timeouts;
    ev_control_parser_reset_keep_link(parser);
    return true;
}

static bool ev_control_parse_header(ev_control_parser_t *parser)
{
    unsigned int metadata_len = 0;
    unsigned int payload_len = 0;
    unsigned int crc = 0;
    int consumed = 0;
    if ((sscanf(parser->header, "EVMUX/1 h=%u p=%u c=%8x\r\n%n",
                &metadata_len, &payload_len, &crc, &consumed) != 3) ||
            (consumed != (int)parser->header_len) || (metadata_len == 0) ||
            (metadata_len > EV_CONTROL_METADATA_MAX) || (payload_len > EV_FILE_PAYLOAD_MAX)) {
        return false;
    }
    parser->metadata_len = metadata_len;
    parser->payload_len = payload_len;
    parser->expected_crc = crc;
    parser->state = EV_CONTROL_PARSE_METADATA;
    return true;
}

static void ev_control_parser_feed_byte(ev_control_parser_t *parser,
                                        const ev_link_ref_t *link,
                                        uint8_t byte,
                                        uint32_t now_ms)
{
    if (ev_control_parser_expire(parser, now_ms)) {
        if (byte == 0x1e) {
            ev_control_parser_begin(parser, link, now_ms);
        }
        return;
    }
    switch (parser->state) {
    case EV_CONTROL_PARSE_SOF:
        if (byte == 0x1e) {
            ev_control_parser_begin(parser, link, now_ms);
        }
        return;
    case EV_CONTROL_PARSE_HEADER:
        if ((byte == 0x1e) && (parser->header_len == 0)) {
            ev_control_parser_begin(parser, link, now_ms);
            return;
        }
        if (parser->header_len + 1 >= sizeof(parser->header)) {
            ++s_stats.frames_bad;
            ev_control_parser_reset_keep_link(parser);
            return;
        }
        parser->header[parser->header_len++] = (char)byte;
        parser->header[parser->header_len] = '\0';
        parser->last_progress_ms = now_ms;
        if ((byte == '\n') && !ev_control_parse_header(parser)) {
            ++s_stats.frames_bad;
            ev_control_parser_reset_keep_link(parser);
        }
        return;
    case EV_CONTROL_PARSE_METADATA:
        if (byte == 0) {
            ++s_stats.frames_bad;
            ev_control_parser_reset_keep_link(parser);
            return;
        }
        parser->metadata[parser->metadata_pos++] = (char)byte;
        parser->last_progress_ms = now_ms;
        if (parser->metadata_pos >= parser->metadata_len) {
            parser->metadata[parser->metadata_len] = '\0';
            if (parser->payload_len > EV_CONTROL_PAYLOAD_MAX) {
                ev_control_json_metadata_t metadata;
                ev_session_snapshot_t session;
                ev_session_result_t result;
                if (!ev_control_json_parse_metadata(parser->metadata, parser->metadata_len, &metadata) ||
                        strcmp(metadata.channel, "user.rpc") != 0 || strcmp(metadata.method, "file.write") != 0 ||
                        strcmp(metadata.type, "req") != 0 || strcmp(metadata.encoding, "binary") != 0 ||
                        !ev_control_frame_authorized(link, metadata.channel, metadata.method, &metadata, "",
                                                     parser->payload_len, &session, &result) || result != EV_SESSION_RESULT_OK) {
                    ++s_stats.frames_rejected;
                    ev_control_parser_reset_keep_link(parser);
                    return;
                }
                parser->file_payload = heap_caps_malloc(parser->payload_len + 1, MALLOC_CAP_8BIT);
                if (parser->file_payload == NULL) {
                    ++s_stats.frames_rejected;
                    ev_control_parser_reset_keep_link(parser);
                    return;
                }
            }
            parser->state = (parser->payload_len == 0) ? EV_CONTROL_PARSE_EOF : EV_CONTROL_PARSE_PAYLOAD;
        }
        return;
    case EV_CONTROL_PARSE_PAYLOAD:
        ev_control_frame_payload(parser)[parser->payload_pos++] = byte;
        parser->last_progress_ms = now_ms;
        if (parser->payload_pos >= parser->payload_len) {
            parser->state = EV_CONTROL_PARSE_EOF;
        }
        return;
    case EV_CONTROL_PARSE_EOF:
        if ((byte == 0x1f) && ((parser->expected_crc == 0) ||
                               (ev_mux_crc32(ev_control_frame_payload(parser), parser->payload_len) == parser->expected_crc))) {
            ++s_stats.frames_ok;
            if (ev_transport_link_is_current(link)) {
                ev_control_handle_frame(parser, link);
            } else {
                ++s_stats.stale_generation_drops;
            }
        } else if (byte == 0x1e) {
            ++s_stats.frames_bad;
            ev_control_parser_begin(parser, link, now_ms);
            return;
        } else {
            ++s_stats.frames_bad;
        }
        ev_control_parser_reset_keep_link(parser);
        return;
    }
    ++s_stats.frames_bad;
    ev_control_parser_reset_keep_link(parser);
}

esp_err_t ev_control_transport_rx(const ev_link_ref_t *link,
                                  const uint8_t *bytes,
                                  size_t length)
{
    if ((link == NULL) || (link->transport_id >= EV_TRANSPORT_MAX) ||
            (link->slot >= EV_TRANSPORT_LINK_SLOTS_MAX) ||
            ((length != 0) && (bytes == NULL)) || (length > 1024)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ev_transport_link_is_current(link)) {
        ++s_stats.stale_generation_drops;
        return ESP_ERR_INVALID_STATE;
    }
    ev_control_parser_t *parser = &s_parsers[link->transport_id][link->slot];
    if (!parser->link_bound || !ev_control_link_equal(&parser->link, link)) {
        ev_control_parser_reset(parser);
        parser->link_bound = true;
        parser->link = *link;
    }
    uint32_t now_ms = ev_control_now_ms();
    (void)ev_control_parser_expire(parser, now_ms);
    if (length == 0) {
        return ESP_OK;
    }
    s_stats.rx_bytes[link->transport_id] += (uint32_t)length;
    for (size_t i = 0; i < length; ++i) {
        ev_control_parser_feed_byte(parser, link, bytes[i], now_ms);
    }
    return ESP_OK;
}

void ev_control_transport_init0(void)
{
    if (s_vm_queue_lock == NULL) {
        s_vm_queue_lock = xSemaphoreCreateMutexStatic(&s_vm_queue_lock_buf);
        if (s_vm_queue_lock == NULL) {
            return;
        }
    }
    if (s_transport_state_lock == NULL) {
        s_transport_state_lock = xSemaphoreCreateMutexStatic(&s_transport_state_lock_buf);
        if (s_transport_state_lock == NULL) {
            return;
        }
    }
    xSemaphoreTake(s_transport_state_lock, portMAX_DELAY);
    if (!s_control_initialized) {
        for (size_t id = 0; id < EV_TRANSPORT_MAX; ++id) {
            for (size_t slot = 0; slot < EV_TRANSPORT_LINK_SLOTS_MAX; ++slot) {
                ev_control_parser_reset(&s_parsers[id][slot]);
            }
        }
        (void)ev_file_transfer_init();
        ev_control_repl_rx_clear();
        ev_control_vm_queue_clear();
        memset(&s_stats, 0, sizeof(s_stats));
        s_control_initialized = true;
        ev_session_set_changed_cb(ev_control_session_changed);
        esp_vision_network_set_changed_callback(ev_control_network_changed, NULL);
    }
    xSemaphoreGive(s_transport_state_lock);
}

void ev_control_transport_start(void)
{
    if (!s_control_initialized || (s_vm_queue_lock == NULL) || (s_transport_state_lock == NULL)) {
        return;
    }
    if (s_transport_task == NULL) {
        // Survives soft resets: physical parsers and the lease timer stay
        // owned by one task while MicroPython rebuilds its VM.
        BaseType_t ok = xTaskCreate(ev_control_transport_task,
                                    "ev_transport",
                                    EV_CONTROL_TRANSPORT_TASK_STACK_SIZE,
                                    NULL,
                                    EV_CONTROL_TRANSPORT_TASK_PRIORITY,
                                    &s_transport_task);
        if (ok != pdPASS) {
            s_transport_task = NULL;
        }
    }
}

bool ev_control_transport_read_pending(int *out_chr)
{
    return ev_control_repl_rx_pop(out_chr);
}

bool ev_control_transport_repl_pending(void)
{
    ev_session_snapshot_t session;
    if (!ev_session_snapshot(&session)) {
        return false;
    }
    bool ready = (session.state == EV_SESSION_OWNED) && session.handshake_complete && session.vm_ready;
    portENTER_CRITICAL(&s_repl_rx_mux);
    bool pending = ev_repl_queue_pending(&s_repl_rx, session.input_generation,
                                         ev_session_input_generation(), ready);
    portEXIT_CRITICAL(&s_repl_rx_mux);
    return pending;
}

void ev_control_transport_vm_poll(void)
{
    ev_control_vm_frame_t frame;
    ev_control_vm_queue_lock();
    if (s_vm_queue_head == s_vm_queue_tail) {
        ev_control_vm_queue_unlock();
        return;
    }
    memcpy(&frame, &s_vm_queue[s_vm_queue_head], sizeof(frame));
    s_vm_queue_head = (uint8_t)((s_vm_queue_head + 1) % EV_CONTROL_VM_QUEUE_LEN);
    ev_control_vm_queue_unlock();

    ev_session_snapshot_t session;
    if ((ev_session_input_generation() != frame.input_generation) ||
            (ev_session_authorize(&frame.link, frame.session_id, true, true,
                                  frame.vm_epoch, ev_control_now_ms(), &session) != EV_SESSION_RESULT_OK) ||
            (session.input_generation != frame.input_generation) ||
            (session.transition_id != frame.transition_id)) {
        ++s_stats.stale_generation_drops;
        return;
    }
    ev_control_rpc_domain_t domain = (ev_control_rpc_domain_t)frame.domain;
    ev_control_response_context_clear();
    s_response_rpc_domain = domain;
    s_response_link = frame.link;
    s_has_response_link = true;
    memcpy(s_response_session_id, frame.session_id, sizeof(s_response_session_id));
    s_response_vm_epoch = frame.vm_epoch;
    s_response_tx_vm_epoch = frame.vm_epoch;
    s_response_input_generation = frame.input_generation;
    s_response_transition_id = frame.transition_id;
    ev_vm_output_origin_t origin;
    ev_vm_output_scope_t output_scope;
    if (!ev_vm_output_origin_from_session(&session, &origin) ||
            !ev_vm_output_scope_push(&origin, &output_scope)) {
        (void)ev_control_send_error(frame.method, frame.seq,
                                    "INTERNAL_ERROR", "VM output context unavailable");
        ev_control_response_context_clear();
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        if (domain == EV_RPC_DEBUG) {
            ev_control_handle_debug_rpc(frame.seq, frame.method, (const char *)frame.payload);
        } else {
            ev_control_handle_user_rpc(frame.seq, frame.method, (const char *)frame.payload);
        }
        nlr_pop();
        ev_vm_output_scope_pop(&output_scope);
        ev_control_response_context_clear();
        return;
    }
    // C handlers may call MicroPython/VFS APIs that raise through NLR. Restore
    // the prior task-local source without throwing into readline's NLR stack.
    // The fixed error text is bounded and stays tied to this request context.
    (void)ev_control_send_error(frame.method, frame.seq,
                                "INTERNAL_ERROR", "VM RPC handler raised");
    ev_vm_output_scope_pop(&output_scope);
    ev_control_response_context_clear();
}

void ev_control_transport_vm_reset_begin(void)
{
    uint32_t old_epoch = ev_session_vm_epoch();
    /* A soft reset changes the Python VM epoch without necessarily changing
     * the authenticated EV-MUX session ID. Any outstanding storage lease is
     * therefore stale and must remain quarantined, not be reusable by the
     * newly initialized VM under the same session. */
    esp_vision_msc_writer_session_changed(NULL);
    esp_vision_launcher_state_vm_reset_begin();
    ev_session_set_vm_ready(false);
    (void)ev_session_vm_epoch_advance();
    if (old_epoch != 0) {
        ev_mux_cancel_vm_epoch(old_epoch);
    }
    ev_control_vm_queue_clear();
    ev_control_repl_rx_clear();
}

void ev_control_transport_vm_ready(void)
{
    ev_vm_output_init0();
    if (ev_session_vm_epoch() == 0) {
        (void)ev_session_vm_epoch_advance();
    }
    ev_session_set_vm_ready(true);
    if (ev_stdio_mux_enabled()) {
        (void)ev_control_transport_send_hello();
    }
}

static void ev_control_transport_poll_serial(void)
{
#if MICROPY_HW_ESP_USB_SERIAL_JTAG
    usb_serial_jtag_poll_rx();
#endif
#if MICROPY_HW_USB_CDC
    mp_usbd_cdc_transport_pump();
#endif
}

#if MICROPY_HW_USB_CDC
static void ev_control_transport_cdc_reset_parser(void)
{
    ev_control_parser_reset(&s_parsers[EV_TRANSPORT_USB][EV_TRANSPORT_PHYSICAL_SLOT]);
}

static void ev_control_transport_cdc_close(void)
{
    ev_link_ref_t link;
    if (ev_transport_link_ref(EV_TRANSPORT_USB, EV_TRANSPORT_PHYSICAL_SLOT, &link)) {
        // Physical USB unmount releases a CDC owner immediately. DTR changes
        // leave the mounted link and its session untouched.
        (void)ev_session_link_lost(&link, EV_SESSION_CHANGE_LINK_LOST);
        ev_control_transport_cdc_reset_parser();
        ev_transport_link_close(&link);
    } else {
        ev_control_transport_cdc_reset_parser();
    }
}

static void ev_control_transport_cdc_open(void)
{
    ev_link_ref_t link;
    if (ev_transport_link_ref(EV_TRANSPORT_USB, EV_TRANSPORT_PHYSICAL_SLOT, &link)) {
        // Startup can find USB already mounted while the static registry
        // still contains its boot generation. Rotate it before accepting RX.
        (void)ev_session_link_lost(&link, EV_SESSION_CHANGE_LINK_LOST);
        ev_link_ref_t refreshed;
        if (!ev_transport_link_advance_generation(&link, &refreshed)) {
            return;
        }
        link = refreshed;
    } else if (ev_transport_link_open(EV_TRANSPORT_USB,
                                      EV_TRANSPORT_PHYSICAL_SLOT, &link) != ESP_OK) {
        return;
    }
    ev_control_transport_cdc_reset_parser();
}

static void ev_control_transport_cdc_update(void)
{
    bool mounted = false;
    bool generation_valid = false;
    uint32_t rx_generation = 0;
    // This snapshot is serialized with the TinyUSB mount/unmount callbacks.
    // In mux mode it filters stale ring bytes while retaining a new frame
    // received after a mount callback in the same USB pump.
    mp_usbd_cdc_transport_rx_state(ev_stdio_mux_enabled(), &mounted,
                                   &rx_generation, &generation_valid);
    mounted = mounted && generation_valid;
    ev_link_ref_t link;
    bool link_open = ev_transport_link_ref(EV_TRANSPORT_USB,
                                           EV_TRANSPORT_PHYSICAL_SLOT, &link);

    if (!s_cdc_connection_known) {
        s_cdc_connection_known = true;
        s_cdc_mounted = mounted;
        s_cdc_rx_generation = rx_generation;
        if (mounted) {
            ev_control_transport_cdc_open();
        } else if (link_open) {
            ev_control_transport_cdc_close();
        }
        if (!mounted) {
            mp_usbd_cdc_transport_clear_rx_if_unmounted();
        }
        return;
    }

    bool generation_changed = rx_generation != s_cdc_rx_generation;
    if (generation_changed || (mounted != s_cdc_mounted)) {
        s_cdc_rx_generation = rx_generation;
        s_cdc_mounted = mounted;
        if (link_open) {
            ev_control_transport_cdc_close();
        }
        if (mounted) {
            ev_control_transport_cdc_open();
        } else {
            mp_usbd_cdc_transport_clear_rx_if_unmounted();
        }
        return;
    }

    if (mounted && !link_open) {
        // Recover if an unrecoverable CDC TX failure closed the generation
        // while USB remains mounted. Retire all input buffered for
        // that generation before opening a fresh transport link.
        if (mp_usbd_cdc_transport_rx_restart(&rx_generation)) {
            s_cdc_rx_generation = rx_generation;
            ev_control_transport_cdc_open();
        } else {
            s_cdc_mounted = false;
            mp_usbd_cdc_transport_clear_rx_if_unmounted();
        }
    } else if (!mounted && link_open) {
        ev_control_transport_cdc_close();
        mp_usbd_cdc_transport_clear_rx_if_unmounted();
    }
}
#endif

static ev_io_result_t ev_control_transport_read_link(const ev_link_ref_t *link,
                                                     void *buffer,
                                                     size_t capacity)
{
#if MICROPY_HW_USB_CDC
    if ((link != NULL) && (link->transport_id == EV_TRANSPORT_USB)) {
        size_t length = mp_usbd_cdc_transport_rx_read(s_cdc_rx_generation,
                                                      buffer, capacity);
        if (length != 0) {
            return (ev_io_result_t) {
                .status = EV_IO_PROGRESS, .transferred = length, .error = ESP_OK
            };
        }
        return (ev_io_result_t) {
            .status = EV_IO_WOULD_BLOCK, .transferred = 0, .error = ESP_OK
        };
    }
#endif
    return ev_transport_read(link, buffer, capacity);
}

// Drain one bounded chunk per compiled serial endpoint. TCP has its own socket
// manager and calls ev_control_transport_rx directly, preserving parser
// ownership and backpressure at recv().
static void ev_control_transport_pump(void)
{
    ev_control_transport_poll_serial();
#if MICROPY_HW_USB_CDC
    ev_control_transport_cdc_update();
#endif
    uint8_t chunk[EV_CONTROL_RX_CHUNK_MAX];
    for (ev_transport_id_t id = EV_TRANSPORT_USJ; id <= EV_TRANSPORT_UART; ++id) {
        ev_link_ref_t link;
        if (!ev_transport_link_ref(id, EV_TRANSPORT_PHYSICAL_SLOT, &link)) {
            continue;
        }
        ev_io_result_t result = ev_control_transport_read_link(&link, chunk, sizeof(chunk));
        if (result.status == EV_IO_PROGRESS) {
            (void)ev_control_transport_rx(&link, chunk, result.transferred);
        } else if (result.status == EV_IO_WOULD_BLOCK) {
            (void)ev_control_transport_rx(&link, NULL, 0);
        }
    }
}

static void ev_control_transport_task(void *arg)
{
    (void)arg;
    TickType_t pump_delay_ticks = pdMS_TO_TICKS(EV_CONTROL_TRANSPORT_PUMP_MS);
    if (pump_delay_ticks == 0) {
        pump_delay_ticks = 1;
    }
    for (;;) {
        if (ev_stdio_mux_enabled()) {
            xSemaphoreTake(s_transport_state_lock, portMAX_DELAY);
            ev_control_transport_pump();
            xSemaphoreGive(s_transport_state_lock);
        } else {
            // The VM owns raw-REPL reads while framed mode is disabled.
            ev_control_transport_poll_serial();
#if MICROPY_HW_USB_CDC
            ev_control_transport_cdc_update();
#endif
        }
        (void)ev_session_tick(ev_control_now_ms());
        ev_file_transfer_poll();
        vTaskDelay(pump_delay_ticks);
    }
}

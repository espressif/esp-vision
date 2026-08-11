/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ev_control_transport.h"

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
#include "py/ringbuf.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "shared/readline/readline.h"

#include "boardconfig.h"
#include "camera.h"
#include "ev_channel.h"
#include "ev_mux.h"
#include "ev_stdio.h"
#include "firmware_info.h"
#include "jpeg.h"
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
#endif

// Byte queues fed by the physical ingress drivers (see mphalport.c).
extern ringbuf_t cdc_stdin_ringbuf;
extern ringbuf_t uart_stdin_ringbuf;

#ifndef ESP_VISION_BOARD_TYPE
#define ESP_VISION_BOARD_TYPE "unknown"
#endif

#ifndef ESP_VISION_BOARD_ARCH
#define ESP_VISION_BOARD_ARCH "unknown"
#endif

#define EV_CONTROL_METADATA_MAX (384)
#define EV_CONTROL_PAYLOAD_MAX  (2048)
#define EV_CONTROL_HEADER_MAX   (72)
#define EV_CONTROL_REPL_RX_MAX  (512)
#define EV_CONTROL_FILE_READ_MAX (384)
#define EV_CONTROL_VM_QUEUE_LEN (2)
#define EV_CONTROL_TRANSPORT_TASK_STACK_SIZE (6 * 1024)
#define EV_CONTROL_TRANSPORT_TASK_PRIORITY (ESP_TASK_PRIO_MIN + 2)
#define EV_CONTROL_TRANSPORT_PUMP_MS (2)

typedef enum {
    EV_CONTROL_PARSE_SOF = 0,
    EV_CONTROL_PARSE_HEADER,
    EV_CONTROL_PARSE_METADATA,
    EV_CONTROL_PARSE_PAYLOAD,
    EV_CONTROL_PARSE_EOF,
} ev_control_parse_state_t;

typedef struct {
    ev_control_parse_state_t state;
    char header[EV_CONTROL_HEADER_MAX];
    size_t header_len;
    char metadata[EV_CONTROL_METADATA_MAX + 1];
    size_t metadata_len;
    size_t metadata_pos;
    uint8_t payload[EV_CONTROL_PAYLOAD_MAX + 1];
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
    uint16_t metadata_len;
    uint16_t payload_len;
    uint8_t domain;
    uint8_t sink;  // ingress the request arrived on; the response must return there
    uint32_t queued_ms;
    char metadata[EV_CONTROL_METADATA_MAX + 1];
    uint8_t payload[EV_CONTROL_PAYLOAD_MAX + 1];
} ev_control_vm_frame_t;

// A queued frame the VM never dispatched (user code never returns to the
// REPL) must not clog the tiny queue forever: entries older than this are
// expired with a VM_TIMEOUT error, checked at push time — which is exactly
// when the host retries.
#define EV_CONTROL_VM_EXPIRE_MS (5000)

// The frame parsers are owned by the transport task (single consumer of the
// physical ingress ring buffers once EV-MUX is enabled).
static ev_control_parser_t s_parsers[EV_CONTROL_INGRESS_MAX];
static uint8_t s_repl_rx[EV_CONTROL_REPL_RX_MAX];
// Producers: transport task (repl.stdin) and VM task (script.run injection).
// Consumer: VM task. Producers serialize on s_repl_rx_mux.
static portMUX_TYPE s_repl_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile size_t s_repl_rx_head;
static volatile size_t s_repl_rx_tail;
static uint32_t s_control_seq;
static portMUX_TYPE s_seq_mux = portMUX_INITIALIZER_UNLOCKED;
// Response routing is per dispatch; each dispatching task keeps its own.
static __thread ev_control_rpc_domain_t s_response_rpc_domain;
// Sink the current request arrived on. debug.rpc answers on the originating
// sink (USJ and CDC are both accepted), independent of stream routing.
static __thread ev_control_ingress_t s_response_sink = EV_CONTROL_INGRESS_USJ;
static ev_control_vm_frame_t s_vm_queue[EV_CONTROL_VM_QUEUE_LEN];
static volatile uint8_t s_vm_queue_head;
static volatile uint8_t s_vm_queue_tail;
static StaticSemaphore_t s_vm_queue_lock_buf;
static SemaphoreHandle_t s_vm_queue_lock;
static StaticSemaphore_t s_transport_state_lock_buf;
static SemaphoreHandle_t s_transport_state_lock;
static TaskHandle_t s_transport_task;

static void ev_control_transport_task(void *arg);
static void ev_control_deliver_ctrl_c(void);
static const char *ev_control_ingress_sink(ev_control_ingress_t ingress);
static bool ev_control_json_get_string(const char *json, const char *key, char *out, size_t out_len);
static uint32_t ev_control_json_get_u32(const char *json, const char *key, uint32_t fallback);
static esp_err_t ev_control_send_error(const char *method, uint32_t seq, const char *code, const char *message);

// Host-visible congestion/health counters (debug.info "transport.stats").
// Written mostly from the transport task, some from ISR contexts; 32-bit
// increments may rarely lose a count under races, which is acceptable here.
typedef struct {
    uint32_t rx_bytes[EV_CONTROL_INGRESS_MAX];
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t frames_rejected;
    uint32_t repl_rx_dropped;
    uint32_t vm_busy;
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

static bool ev_control_repl_rx_push(const uint8_t *data, size_t len)
{
    bool ok = true;
    portENTER_CRITICAL(&s_repl_rx_mux);
    for (size_t i = 0; i < len; ++i) {
        size_t next_tail = (s_repl_rx_tail + 1) % sizeof(s_repl_rx);
        if (next_tail == s_repl_rx_head) {
            ok = false;
            break;
        }
        s_repl_rx[s_repl_rx_tail] = data[i];
        s_repl_rx_tail = next_tail;
    }
    portEXIT_CRITICAL(&s_repl_rx_mux);
    return ok;
}

static bool ev_control_repl_rx_pop(int *out_chr)
{
    bool available = false;
    portENTER_CRITICAL(&s_repl_rx_mux);
    if (s_repl_rx_head != s_repl_rx_tail) {
        *out_chr = s_repl_rx[s_repl_rx_head];
        s_repl_rx_head = (s_repl_rx_head + 1) % sizeof(s_repl_rx);
        available = true;
    }
    portEXIT_CRITICAL(&s_repl_rx_mux);
    return available;
}

// Expire undispatched frames at the head of the queue before every push.
static void ev_control_vm_queue_expire(void)
{
    for (;;) {
        char metadata[EV_CONTROL_METADATA_MAX + 1];
        ev_control_rpc_domain_t domain;
        ev_control_ingress_t sink;

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
        memcpy(metadata, frame->metadata, frame->metadata_len + 1);
        domain = (ev_control_rpc_domain_t)frame->domain;
        sink = (ev_control_ingress_t)frame->sink;
        s_vm_queue_head = (uint8_t)((s_vm_queue_head + 1) % EV_CONTROL_VM_QUEUE_LEN);
        ev_control_vm_queue_unlock();

        char method[32] = "unknown";
        (void)ev_control_json_get_string(metadata, "method", method, sizeof(method));
        uint32_t seq = ev_control_json_get_u32(metadata, "seq", 0);
        ev_control_rpc_domain_t prev_domain = s_response_rpc_domain;
        ev_control_ingress_t prev_sink = s_response_sink;
        s_response_rpc_domain = domain;
        s_response_sink = sink;
        (void)ev_control_send_error(method, seq, "VM_TIMEOUT", "interpreter busy");
        s_response_rpc_domain = prev_domain;
        s_response_sink = prev_sink;
    }
}

static bool ev_control_vm_queue_push(ev_control_rpc_domain_t domain,
                                     ev_control_ingress_t sink,
                                     const char *metadata,
                                     size_t metadata_len,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    ev_control_vm_queue_expire();
    ev_control_vm_queue_lock();
    uint8_t next_tail = (uint8_t)((s_vm_queue_tail + 1) % EV_CONTROL_VM_QUEUE_LEN);
    if (next_tail == s_vm_queue_head) {
        ev_control_vm_queue_unlock();
        return false;
    }
    ev_control_vm_frame_t *frame = &s_vm_queue[s_vm_queue_tail];
    frame->metadata_len = (uint16_t)metadata_len;
    frame->payload_len = (uint16_t)payload_len;
    frame->domain = (uint8_t)domain;
    frame->sink = (uint8_t)sink;
    frame->queued_ms = ev_control_now_ms();
    memcpy(frame->metadata, metadata, metadata_len);
    frame->metadata[metadata_len] = '\0';
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
    if (strncmp(pos, "true", 4) == 0) {
        return true;
    }
    if (strncmp(pos, "false", 5) == 0) {
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

static bool ev_control_vfs_exists(const char *path)
{
    return mp_vfs_import_stat(path) != MP_IMPORT_STAT_NO_EXIST;
}

static esp_err_t ev_control_vfs_remove_if_exists(const char *path)
{
    if (!ev_control_vfs_exists(path)) {
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

static esp_err_t ev_control_send_rpc_with_domain(ev_control_rpc_domain_t domain,
                                                 const char *type,
                                                 const char *method,
                                                 const char *payload,
                                                 uint32_t seq)
{
    char metadata[EV_CONTROL_METADATA_MAX];
    int metadata_len = snprintf(metadata,
                                sizeof(metadata),
                                "{\"sid\":\"%s\",\"seq\":%" PRIu32 ",\"channel\":\"%s\","
                                "\"type\":\"%s\",\"method\":\"%s\",\"encoding\":\"json\",\"ts_ms\":%" PRIu32 "}",
                                (domain == EV_RPC_DEBUG) ? "debug" : "user",
                                seq,
                                ev_control_rpc_channel(domain),
                                type,
                                method,
                                ev_control_now_ms());
    if ((metadata_len <= 0) || ((size_t)metadata_len >= sizeof(metadata))) {
        return ESP_ERR_INVALID_SIZE;
    }
    // RPC responses return to the request ingress. Events follow their
    // stream's current route.
    if (strcmp(type, "rsp") == 0) {
        return ev_mux_write_sink_if_present(ev_control_ingress_sink(s_response_sink),
                                            metadata, payload, strlen(payload));
    }
    return ev_mux_write(ev_control_rpc_stream(domain), metadata, payload, strlen(payload));
}

static esp_err_t ev_control_send_rpc_with_seq(const char *type,
                                              const char *method,
                                              const char *payload,
                                              uint32_t seq)
{
    return ev_control_send_rpc_with_domain(s_response_rpc_domain, type, method, payload, seq);
}

// Binary RPC response (debug.capture_frame): the payload travels inside the
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
    const char *sink_id = ev_control_ingress_sink(s_response_sink);
    char metadata[EV_CONTROL_METADATA_MAX];
    int metadata_len = snprintf(metadata,
                                sizeof(metadata),
                                "{\"sid\":\"debug\",\"seq\":%" PRIu32 ",\"channel\":\"debug.rpc\","
                                "\"type\":\"rsp\",\"method\":\"%s\",\"encoding\":\"binary\","
                                "\"contentType\":\"%s\",\"width\":%" PRIi32 ",\"height\":%" PRIi32 ","
                                "\"ts_ms\":%" PRIu32 "}",
                                seq,
                                method,
                                content_type,
                                width,
                                height,
                                ev_control_now_ms());
    if ((metadata_len <= 0) || ((size_t)metadata_len >= sizeof(metadata))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_mux_write_sink_if_present(sink_id, metadata, payload, payload_len);
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

static void ev_control_route_changed_cb(ev_stream_t stream, const char *sink_id, bool auto_route, const char *reason)
{
    char payload[192];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"stream\":\"%s\",\"route\":\"%s\",\"auto\":%s,\"reason\":\"%s\"}",
                       ev_control_stream_name(stream),
                       sink_id,
                       ev_control_bool_str(auto_route),
                       reason);
    if ((len > 0) && ((size_t)len < sizeof(payload))) {
        (void)ev_control_send_rpc_with_domain(EV_RPC_USER, "event", "route.changed", payload, ev_control_next_seq());
    }
}

static esp_err_t ev_control_send_hello(const char *type, uint32_t seq)
{
    char payload[512];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"device\":\"esp-vision\","
                       "\"firmware\":{\"id\":\"%s\",\"version\":\"%s\"},"
                       "\"board\":\"%s\",\"arch\":\"%s\","
                       "\"idf\":\"%s\",\"evMuxVersion\":3}",
                       ESP_VISION_FIRMWARE_ID,
                       ESP_VISION_FIRMWARE_VERSION,
                       ESP_VISION_BOARD_TYPE,
                       ESP_VISION_BOARD_ARCH,
                       esp_get_idf_version());
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_domain(EV_RPC_USER, type, "hello", payload, seq);
}

esp_err_t ev_control_transport_send_hello(void)
{
    return ev_control_send_hello("event", ev_control_next_seq());
}

static esp_err_t ev_control_send_capabilities(uint32_t seq)
{
    char payload[1024];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"device\":\"esp-vision\","
                       "\"firmware\":{\"id\":\"%s\",\"version\":\"%s\"},"
                       "\"board\":\"%s\",\"arch\":\"%s\","
                       "\"protocol\":{\"evMuxVersion\":3},\"usbProduct\":\"%s\","
                       "\"sinks\":["
                       "{\"id\":\"usj\",\"kind\":\"usb_serial_jtag\",\"state\":\"%s\",\"ready\":%s},"
                       "{\"id\":\"cdc\",\"kind\":\"usb_otg_cdc\",\"state\":\"%s\",\"ready\":%s},"
                       "{\"id\":\"uart\",\"kind\":\"uart\",\"state\":\"%s\",\"ready\":%s}],"
                       "\"streams\":[\"user\",\"debug\"],"
                       "\"channels\":[\"user.rpc\",\"debug.rpc\",\"repl.stdin\",\"repl.stdout\",\"repl.stderr\","
                       "\"repl.signal\",\"log.idf\",\"preview.frame\"],"
                       "\"features\":[\"hello\",\"capabilities\",\"transport.state\",\"route.get\","
                       "\"route.changed\",\"route.bind\",\"route.auto\",\"framed.repl\","
                       "\"script.write\",\"script.write.chunked\",\"script.run\","
                       "\"debug.info\",\"debug.capture_frame\","
                       "\"device.control\",\"dtr.routing\"]}",
                       ESP_VISION_FIRMWARE_ID,
                       ESP_VISION_FIRMWARE_VERSION,
                       ESP_VISION_BOARD_TYPE,
                       ESP_VISION_BOARD_ARCH,
                       MICROPY_HW_USB_PRODUCT_FS_STRING,
                       ev_channel_sink_state("usj"),
                       ev_control_bool_str(ev_channel_sink_ready("usj")),
                       ev_channel_sink_state("cdc"),
                       ev_control_bool_str(ev_channel_sink_ready("cdc")),
                       ev_channel_sink_state("uart"),
                       ev_control_bool_str(ev_channel_sink_ready("uart")));
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "capabilities", payload, seq);
}

static esp_err_t ev_control_send_transport_state(uint32_t seq)
{
    char payload[640];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"user\":{\"route\":\"%s\",\"auto\":%s,\"ready\":%s},"
                       "\"debug\":{\"route\":\"%s\",\"auto\":%s,\"ready\":%s},"
                       "\"sinks\":{\"usj\":{\"state\":\"%s\",\"ready\":%s},"
                       "\"cdc\":{\"state\":\"%s\",\"ready\":%s,\"present\":%s},"
                       "\"uart\":{\"state\":\"%s\",\"ready\":%s}}}",
                       ev_channel_get(EV_STREAM_USER),
                       ev_control_bool_str(ev_channel_is_auto(EV_STREAM_USER)),
                       ev_control_bool_str(ev_channel_ready(EV_STREAM_USER)),
                       ev_channel_get(EV_STREAM_DEBUG),
                       ev_control_bool_str(ev_channel_is_auto(EV_STREAM_DEBUG)),
                       ev_control_bool_str(ev_channel_ready(EV_STREAM_DEBUG)),
                       ev_channel_sink_state("usj"),
                       ev_control_bool_str(ev_channel_sink_ready("usj")),
                       ev_channel_sink_state("cdc"),
                       ev_control_bool_str(ev_channel_sink_ready("cdc")),
                       ev_control_bool_str(ev_channel_sink_present("cdc")),
                       ev_channel_sink_state("uart"),
                       ev_control_bool_str(ev_channel_sink_ready("uart")));
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
                       ev_control_bool_str(ev_channel_is_auto(stream)),
                       ev_control_bool_str(ev_channel_ready(stream)));
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ev_control_send_rpc_with_seq("rsp", "route.get", payload, seq);
}

static esp_err_t ev_control_send_route_bind(uint32_t seq, const char *request)
{
    char stream_name[16] = { 0 };
    char sink[16] = { 0 };
    if (!ev_control_json_get_string(request, "stream", stream_name, sizeof(stream_name))) {
        return ev_control_send_error("route.bind", seq, "INVALID_ARGUMENT", "missing stream");
    }
    if (!ev_control_json_get_string(request, "sink", sink, sizeof(sink))) {
        return ev_control_send_error("route.bind", seq, "INVALID_ARGUMENT", "missing sink");
    }

    ev_stream_t stream = ev_control_stream_from_name(stream_name);
    if (stream == EV_STREAM_MAX) {
        return ev_control_send_error("route.bind", seq, "INVALID_STREAM", "unknown stream");
    }
    esp_err_t ret = ev_channel_bind(stream, sink);
    if (ret != ESP_OK) {
        return ev_control_send_error("route.bind", seq, "ROUTE_BIND_FAILED", esp_err_to_name(ret));
    }
    return ev_control_send_route_get(seq, request);
}

static esp_err_t ev_control_send_route_auto(uint32_t seq, const char *request)
{
    char stream_name[16] = { 0 };
    if (!ev_control_json_get_string(request, "stream", stream_name, sizeof(stream_name))) {
        return ev_control_send_error("route.auto", seq, "INVALID_ARGUMENT", "missing stream");
    }
    ev_stream_t stream = ev_control_stream_from_name(stream_name);
    if (stream == EV_STREAM_MAX) {
        return ev_control_send_error("route.auto", seq, "INVALID_STREAM", "unknown stream");
    }
    esp_err_t ret = ev_channel_bind_auto(stream);
    if (ret != ESP_OK) {
        return ev_control_send_error("route.auto", seq, "ROUTE_AUTO_FAILED", esp_err_to_name(ret));
    }
    return ev_control_send_route_get(seq, request);
}

static esp_err_t ev_control_send_script_write(uint32_t seq, const char *request)
{
    char path[128];
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
    if ((strcmp(mode, "create") == 0) && ev_control_vfs_exists(path)) {
        return ev_control_send_error("script.write", seq, "FILE_EXISTS", "target already exists");
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

    char tmp_path[160];
    int tmp_len = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
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
        (void)ev_control_vfs_remove_if_exists(tmp_path);
        ret = ev_control_vfs_write_file(tmp_path, decoded, decoded_len);
    } else {
        if (!ev_control_vfs_exists(tmp_path)) {
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
            (void)ev_control_repl_rx_push((const uint8_t *)repl_cmd, (size_t)repl_len);
        }
    }

    char payload[160];
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
    char path[128];
    if (!ev_control_json_get_string(request, "path", path, sizeof(path))) {
        return ev_control_send_error("script.run", seq, "INVALID_ARGUMENT", "missing path");
    }
    if (!ev_control_path_allowed(path)) {
        return ev_control_send_error("script.run", seq, "INVALID_PATH", "path is not allowed");
    }

    if (!ev_control_vfs_exists(path)) {
        return ev_control_send_error("script.run", seq, "NOT_FOUND", "script not found");
    }

    char repl_cmd[192];
    int repl_len = snprintf(repl_cmd, sizeof(repl_cmd), "execfile(%c%s%c)\r\n", '"', path, '"');
    if ((repl_len <= 0) || ((size_t)repl_len >= sizeof(repl_cmd))) {
        return ev_control_send_error("script.run", seq, "PATH_TOO_LONG", "script path too long");
    }
    if (!ev_control_repl_rx_push((const uint8_t *)repl_cmd, (size_t)repl_len)) {
        return ev_control_send_error("script.run", seq, "REPL_BUSY", "failed to enqueue script");
    }

    char payload[160];
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
                       "\"firmware\":{\"id\":\"%s\",\"version\":\"%s\"},"
                       "\"board\":\"%s\","
                       "\"arch\":\"%s\",\"idf\":\"%s\","
                       "\"protocol\":{\"evMuxVersion\":3}}",
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
    char path[128] = "/";
    (void)ev_control_json_get_string(request, "path", path, sizeof(path));
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
    char path[128];
    if (!ev_control_json_get_string(request, "path", path, sizeof(path))) {
        return ev_control_send_error("debug.info", seq, "INVALID_ARGUMENT", "missing path");
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
    char payload[512];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"scope\":\"transport.stats\","
                       "\"rx\":{\"usj\":%" PRIu32 ",\"cdc\":%" PRIu32 ",\"uart\":%" PRIu32 "},"
                       "\"ringFull\":{\"usj\":%" PRIu32 ",\"cdc\":%" PRIu32 ",\"uart\":%" PRIu32 "},"
                       "\"frames\":{\"ok\":%" PRIu32 ",\"bad\":%" PRIu32 ",\"rejected\":%" PRIu32 "},"
                       "\"drops\":{\"replRx\":%" PRIu32 ",\"vmBusy\":%" PRIu32 ",\"txTimeout\":%" PRIu32 ",\"txDrop\":%" PRIu32 "}}",
                       s_stats.rx_bytes[EV_CONTROL_INGRESS_USJ],
                       s_stats.rx_bytes[EV_CONTROL_INGRESS_CDC],
                       s_stats.rx_bytes[EV_CONTROL_INGRESS_UART],
                       ev_transport_rx_ring_full[EV_CONTROL_INGRESS_USJ],
                       ev_transport_rx_ring_full[EV_CONTROL_INGRESS_CDC],
                       ev_transport_rx_ring_full[EV_CONTROL_INGRESS_UART],
                       s_stats.frames_ok,
                       s_stats.frames_bad,
                       s_stats.frames_rejected,
                       s_stats.repl_rx_dropped,
                       s_stats.vm_busy,
                       ev_mux_tx_timeout_count(),
                       ev_mux_tx_drop_count());
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
    uint8_t *pixels = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        pixels = heap_caps_malloc(frame_size, MALLOC_CAP_8BIT);
    }
    if (pixels == NULL) {
        return ev_control_send_error("debug.capture_frame", seq, "NO_MEM", "failed to allocate frame buffer");
    }

    image_t img;
    esp_err_t ret = esp_vision_camera_snapshot(&img, pixels, frame_size);
    if (ret != ESP_OK) {
        heap_caps_free(pixels);
        return ev_control_send_error("debug.capture_frame", seq, "CAPTURE_FAILED", esp_err_to_name(ret));
    }

    uint8_t *jpeg_buf = NULL;
    size_t jpeg_size = 0;
    ret = esp_vision_jpeg_encode(&img, quality, &jpeg_buf, &jpeg_size);
    heap_caps_free(pixels);
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

static esp_err_t ev_control_send_device_control(uint32_t seq, const char *request)
{
    char action[24];
    if (!ev_control_json_get_string(request, "action", action, sizeof(action))) {
        return ev_control_send_error("device.control", seq, "INVALID_ARGUMENT", "missing action");
    }
    if (strcmp(action, "status") == 0) {
        char payload[512];
        int len = snprintf(payload,
                           sizeof(payload),
                           "{\"ok\":true,\"action\":\"status\",\"uptimeMs\":%" PRIu32 ","
                           "\"user\":{\"route\":\"%s\",\"auto\":%s,\"ready\":%s},"
                           "\"debug\":{\"route\":\"%s\",\"auto\":%s,\"ready\":%s},"
                           "\"heapFree\":%u}",
                           ev_control_now_ms(),
                           ev_channel_get(EV_STREAM_USER),
                           ev_control_bool_str(ev_channel_is_auto(EV_STREAM_USER)),
                           ev_control_bool_str(ev_channel_ready(EV_STREAM_USER)),
                           ev_channel_get(EV_STREAM_DEBUG),
                           ev_control_bool_str(ev_channel_is_auto(EV_STREAM_DEBUG)),
                           ev_control_bool_str(ev_channel_ready(EV_STREAM_DEBUG)),
                           (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        if ((len <= 0) || ((size_t)len >= sizeof(payload))) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ev_control_send_rpc_with_seq("rsp", "device.control", payload, seq);
    }
    if (strcmp(action, "stop") == 0) {
        ev_control_deliver_ctrl_c();
        return ev_control_send_rpc_with_seq("rsp", "device.control", "{\"ok\":true,\"action\":\"stop\"}", seq);
    }
    if (strcmp(action, "reset") == 0) {
        esp_err_t ret = ev_control_send_rpc_with_seq("rsp", "device.control", "{\"ok\":true,\"action\":\"reset\"}", seq);
        esp_restart();
        return ret;
    }
    return ev_control_send_error("device.control", seq, "INVALID_ACTION", "unsupported action");
}

static void ev_control_handle_user_rpc(uint32_t seq, const char *method, const char *payload)
{
    if (strcmp(method, "hello") == 0) {
        (void)ev_control_send_hello("rsp", seq);
    } else if (strcmp(method, "capabilities") == 0) {
        (void)ev_control_send_capabilities(seq);
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
    } else {
        (void)ev_control_send_error(method, seq, "UNKNOWN_METHOD", "unsupported debug RPC method");
    }
}

static const char *ev_control_ingress_sink(ev_control_ingress_t ingress)
{
    switch (ingress) {
    case EV_CONTROL_INGRESS_USJ:
        return "usj";
    case EV_CONTROL_INGRESS_CDC:
        return "cdc";
    case EV_CONTROL_INGRESS_UART:
        return "uart";
    default:
        return "unknown";
    }
}

static bool ev_control_is_bootstrap_method(const char *method)
{
    return (strcmp(method, "hello") == 0) ||
           (strcmp(method, "capabilities") == 0);
}

static void ev_control_log_rejected_frame(ev_control_ingress_t ingress,
                                          const char *channel,
                                          const char *method)
{
    ++s_stats.frames_rejected;
    char metadata[EV_CONTROL_METADATA_MAX];
    char payload[160];
    uint32_t seq = ev_control_next_seq();
    int metadata_len = snprintf(metadata,
                                sizeof(metadata),
                                "{\"sid\":\"system\",\"seq\":%" PRIu32 ",\"channel\":\"log.idf\","
                                "\"type\":\"data\",\"method\":\"transport\",\"encoding\":\"text\","
                                "\"ts_ms\":%" PRIu32 "}",
                                seq,
                                ev_control_now_ms());
    int payload_len = snprintf(payload,
                               sizeof(payload),
                               "reject ingress=%s channel=%s method=%s route=%s\n",
                               ev_control_ingress_sink(ingress),
                               channel,
                               (method[0] != '\0') ? method : "<missing>",
                               ev_channel_get((strcmp(channel, "debug.rpc") == 0) ?
                                              EV_STREAM_DEBUG : EV_STREAM_USER));
    if ((metadata_len > 0) && ((size_t)metadata_len < sizeof(metadata)) &&
            (payload_len > 0) && ((size_t)payload_len < sizeof(payload))) {
        (void)ev_mux_write(EV_STREAM_DEBUG, metadata, payload, (size_t)payload_len);
    }
}

static bool ev_control_frame_authorized(ev_control_ingress_t ingress,
                                        const char *channel,
                                        const char *method)
{
    ev_channel_poll_auto();
    const char *sink_id = ev_control_ingress_sink(ingress);
    // Discovery methods are accepted on either USB sink.
    if ((strcmp(channel, "user.rpc") == 0) && ev_control_is_bootstrap_method(method)) {
        return (ingress == EV_CONTROL_INGRESS_USJ) || (ingress == EV_CONTROL_INGRESS_CDC);
    }
    if (strcmp(channel, "debug.rpc") == 0) {
        return strcmp(sink_id, ev_channel_get(EV_STREAM_DEBUG)) == 0;
    }
    if ((strcmp(channel, "user.rpc") == 0) ||
            (strcmp(channel, "repl.stdin") == 0) ||
            (strcmp(channel, "repl.signal") == 0)) {
        return strcmp(sink_id, ev_channel_get(EV_STREAM_USER)) == 0;
    }
    return false;
}

// RPC methods that touch MicroPython objects, the VFS or the camera must run
// on the VM task; everything else is answered directly on the transport task.
static bool ev_control_rpc_needs_vm(ev_control_rpc_domain_t domain, const char *method, const char *payload)
{
    if (domain == EV_RPC_USER) {
        return (strcmp(method, "script.write") == 0) ||
               (strcmp(method, "script.run") == 0);
    }
    if (strcmp(method, "debug.capture_frame") == 0) {
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
static void ev_control_deliver_ctrl_c(void)
{
    if (mp_interrupt_char == CHAR_CTRL_C) {
        mp_sched_keyboard_interrupt();
    } else {
        (void)ev_control_repl_rx_push((const uint8_t *)"\x03", 1);
    }
    mp_hal_wake_main_task();
}

static void ev_control_handle_frame(ev_control_parser_t *parser, ev_control_ingress_t ingress)
{
    char channel[24] = { 0 };
    char method[32] = { 0 };
    uint32_t seq = ev_control_json_get_u32(parser->metadata, "seq", ev_control_next_seq());

    if (!ev_control_json_get_string(parser->metadata, "channel", channel, sizeof(channel))) {
        return;
    }
    parser->payload[parser->payload_len] = '\0';

    bool is_rpc = (strcmp(channel, "user.rpc") == 0) || (strcmp(channel, "debug.rpc") == 0);
    bool has_method = !is_rpc || ev_control_json_get_string(parser->metadata, "method", method, sizeof(method));
    if (!ev_control_frame_authorized(ingress, channel, method)) {
        ev_control_log_rejected_frame(ingress, channel, has_method ? method : "");
        return;
    }

    if (strcmp(channel, "repl.stdin") == 0) {
        if (!ev_control_repl_rx_push(parser->payload, parser->payload_len)) {
            ++s_stats.repl_rx_dropped;
        }
        mp_hal_wake_main_task();
        return;
    }
    if (strcmp(channel, "repl.signal") == 0) {
        if ((parser->payload_len == 1) && (parser->payload[0] == 0x03)) {
            ev_control_deliver_ctrl_c();
        }
        return;
    }
    if (is_rpc) {
        ev_control_rpc_domain_t domain = (strcmp(channel, "debug.rpc") == 0) ? EV_RPC_DEBUG : EV_RPC_USER;
        s_response_rpc_domain = domain;
        s_response_sink = ingress;
        if (!has_method) {
            (void)ev_control_send_error("unknown", seq, "INVALID_FRAME", "missing method");
            s_response_rpc_domain = EV_RPC_USER;
            s_response_sink = EV_CONTROL_INGRESS_USJ;
            return;
        }
        if (ev_control_rpc_needs_vm(domain, method, (const char *)parser->payload)) {
            if (!ev_control_vm_queue_push(domain, ingress, parser->metadata, parser->metadata_len,
                                          parser->payload, parser->payload_len)) {
                ++s_stats.vm_busy;
                (void)ev_control_send_error(method, seq, "VM_BUSY", "interpreter busy");
            }
            s_response_rpc_domain = EV_RPC_USER;
            s_response_sink = EV_CONTROL_INGRESS_USJ;
            mp_hal_wake_main_task();
            return;
        }
        if (domain == EV_RPC_DEBUG) {
            ev_control_handle_debug_rpc(seq, method, (const char *)parser->payload);
        } else {
            ev_control_handle_user_rpc(seq, method, (const char *)parser->payload);
        }
        s_response_rpc_domain = EV_RPC_USER;
        s_response_sink = EV_CONTROL_INGRESS_USJ;
    }
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
    ev_control_vm_queue_lock();
    for (size_t i = 0; i < EV_CONTROL_INGRESS_MAX; ++i) {
        ev_control_parser_reset(&s_parsers[i]);
    }
    portENTER_CRITICAL(&s_repl_rx_mux);
    s_repl_rx_head = 0;
    s_repl_rx_tail = 0;
    portEXIT_CRITICAL(&s_repl_rx_mux);
    s_vm_queue_head = 0;
    s_vm_queue_tail = 0;
    ev_control_vm_queue_unlock();
    xSemaphoreGive(s_transport_state_lock);
    ev_channel_set_route_changed_cb(ev_control_route_changed_cb);
}

void ev_control_transport_start(void)
{
    if ((s_vm_queue_lock == NULL) || (s_transport_state_lock == NULL)) {
        return;
    }
    if (s_transport_task == NULL) {
        // Survives soft resets: frame reception must keep working while the
        // VM restarts, so the task is created once and never deleted. The
        // caller starts it only after TinyUSB has completed initialization.
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
    portENTER_CRITICAL(&s_repl_rx_mux);
    bool pending = s_repl_rx_head != s_repl_rx_tail;
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
    ev_control_rpc_domain_t domain = (ev_control_rpc_domain_t)frame.domain;

    char method[32] = { 0 };
    uint32_t seq = ev_control_json_get_u32(frame.metadata, "seq", ev_control_next_seq());
    s_response_rpc_domain = domain;
    s_response_sink = (ev_control_ingress_t)frame.sink;
    if (!ev_control_json_get_string(frame.metadata, "method", method, sizeof(method))) {
        (void)ev_control_send_error("unknown", seq, "INVALID_FRAME", "missing method");
    } else if (domain == EV_RPC_DEBUG) {
        ev_control_handle_debug_rpc(seq, method, (const char *)frame.payload);
    } else {
        ev_control_handle_user_rpc(seq, method, (const char *)frame.payload);
    }
    s_response_rpc_domain = EV_RPC_USER;
    s_response_sink = EV_CONTROL_INGRESS_USJ;
}

// Parser feed, transport task only. REPL bytes reach the VM task through
// s_repl_rx (see ev_control_transport_read_pending).
static void ev_control_transport_rx_chr(ev_control_ingress_t ingress, uint8_t c)
{
    if (ingress >= EV_CONTROL_INGRESS_MAX) {
        return;
    }
    ev_control_parser_t *parser = &s_parsers[ingress];

    switch (parser->state) {
    case EV_CONTROL_PARSE_SOF:
        if (c == 0x1e) {
            ev_control_parser_reset(parser);
            parser->state = EV_CONTROL_PARSE_HEADER;
        }
        return;

    case EV_CONTROL_PARSE_HEADER:
        if (parser->header_len + 1 >= sizeof(parser->header)) {
            ++s_stats.frames_bad;
            ev_control_parser_reset(parser);
            return;
        }
        parser->header[parser->header_len++] = (char)c;
        parser->header[parser->header_len] = '\0';
        if (c == '\n') {
            unsigned int metadata_len = 0;
            unsigned int payload_len = 0;
            unsigned int crc = 0;
            int consumed = 0;
            if ((sscanf(parser->header,
                        "EVMUX/1 h=%u p=%u c=%8x\r\n%n",
                        &metadata_len,
                        &payload_len,
                        &crc,
                        &consumed) != 3) ||
                    (consumed != (int)parser->header_len)) {
                ++s_stats.frames_bad;
                ev_control_parser_reset(parser);
                return;
            }
            if ((metadata_len == 0) || (metadata_len > EV_CONTROL_METADATA_MAX) ||
                    (payload_len > EV_CONTROL_PAYLOAD_MAX)) {
                ++s_stats.frames_bad;
                ev_control_parser_reset(parser);
                return;
            }
            parser->metadata_len = metadata_len;
            parser->payload_len = payload_len;
            parser->expected_crc = crc;
            parser->state = EV_CONTROL_PARSE_METADATA;
        }
        return;

    case EV_CONTROL_PARSE_METADATA:
        parser->metadata[parser->metadata_pos++] = (char)c;
        if (parser->metadata_pos >= parser->metadata_len) {
            parser->metadata[parser->metadata_len] = '\0';
            parser->state = (parser->payload_len == 0) ? EV_CONTROL_PARSE_EOF : EV_CONTROL_PARSE_PAYLOAD;
        }
        return;

    case EV_CONTROL_PARSE_PAYLOAD:
        parser->payload[parser->payload_pos++] = c;
        if (parser->payload_pos >= parser->payload_len) {
            parser->state = EV_CONTROL_PARSE_EOF;
        }
        return;

    case EV_CONTROL_PARSE_EOF:
        if (c == 0x1f) {
            if ((parser->expected_crc == 0) ||
                    (ev_mux_crc32(parser->payload, parser->payload_len) == parser->expected_crc)) {
                ++s_stats.frames_ok;
                ev_control_handle_frame(parser, ingress);
            } else {
                ++s_stats.frames_bad;
            }
        } else {
            ++s_stats.frames_bad;
        }
        ev_control_parser_reset(parser);
        return;
    }

    ++s_stats.frames_bad;
    ev_control_parser_reset(parser);
}

// Drain every physical ingress into its frame parser, then maintain routes.
// Runs on the transport task while EV-MUX owns the byte stream.
static void ev_control_transport_pump(void)
{
#if MICROPY_HW_ESP_USB_SERIAL_JTAG
    usb_serial_jtag_poll_rx();
#endif
#if MICROPY_HW_USB_CDC
    mp_usbd_cdc_transport_pump();
#endif

    static ringbuf_t *const s_ingress_bufs[EV_CONTROL_INGRESS_MAX] = {
        &stdin_ringbuf,
        &cdc_stdin_ringbuf,
        &uart_stdin_ringbuf,
    };
    for (size_t ingress = 0; ingress < EV_CONTROL_INGRESS_MAX; ++ingress) {
        int c;
        while ((c = ringbuf_get(s_ingress_bufs[ingress])) != -1) {
            ++s_stats.rx_bytes[ingress];
            ev_control_transport_rx_chr((ev_control_ingress_t)ingress, (uint8_t)c);
        }
    }

    // Route maintenance is independent of VM execution.
    ev_channel_poll_auto();
}

// Emit hello whenever a CDC link appears so the control plane stays
// discoverable without any REPL interaction (boot-default EV-MUX).
static void ev_control_transport_watch_hello(void)
{
#if MICROPY_HW_USB_CDC
    static bool s_cdc_was_present;
    bool present = ev_channel_sink_present("cdc");
    if (present && !s_cdc_was_present && ev_stdio_mux_enabled()) {
        (void)ev_control_transport_send_hello();
    }
    s_cdc_was_present = present;
#endif
}

static void ev_control_transport_task(void *arg)
{
    (void)arg;
    TickType_t pump_delay_ticks = pdMS_TO_TICKS(EV_CONTROL_TRANSPORT_PUMP_MS);
    if (pump_delay_ticks == 0) {
        pump_delay_ticks = 1;
    }
    for (;;) {
        ev_control_transport_watch_hello();
        if (ev_stdio_mux_enabled()) {
            xSemaphoreTake(s_transport_state_lock, portMAX_DELAY);
            ev_control_transport_pump();
            xSemaphoreGive(s_transport_state_lock);
        } else {
            // EV-MUX off: the main task drives the parsers through
            // mp_hal_stdin_rx_chr(), but keep the USB endpoints moving so a
            // raw Ctrl-C still lands during long C calls.
#if MICROPY_HW_ESP_USB_SERIAL_JTAG
            usb_serial_jtag_poll_rx();
#endif
#if MICROPY_HW_USB_CDC
            mp_usbd_cdc_transport_pump();
#endif
        }
        vTaskDelay(pump_delay_ticks);
    }
}

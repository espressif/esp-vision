/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "irisfw_acceptance.h"
#include "irisfw_boot.h"
#include "firmware_status.h"
#include "firmware_info.h"
#include "device_identity.h"
#include "ev_mux.h"
#include "ev_session.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

/* Installer-only subset of the public Iris 0.2 console contract. No Iris
 * component, second USB stack, REPL command, updater or recovery-entry RPC.
 * All input/state belongs to the existing EV transport task. All output uses
 * its shared TX scheduler, preserving whole EV-MUX frame boundaries.
 */
enum {
    ACCEPT_PAYLOAD_MAX = 512,
    ACCEPT_HEADER_SIZE = 32,
    ACCEPT_LINE_SIZE = 768,
    CH_CONTROL = 0, CH_EVENT = 2, CH_UPDATE = 9,
    CT_HELLO = 1, CT_ACK = 2, CT_PING = 3, CT_PONG = 4,
    CT_TIME = 5, CT_TIME_REPLY = 6, CT_STATUS = 7, CT_STATUS_REPLY = 8,
    CT_CREDIT = 9, CT_AUTH = 0x16, CT_ERROR = 0x7f,
    EV_BOOT = 1, EV_HEALTHY = 3, EV_LINK_READY = 5,
    UPDATE_INVENTORY = 0x0e, UPDATE_INVENTORY_REPLY = 0x0f,
    TAG_DEVICE = 1, TAG_BOOT = 2, TAG_UPTIME = 3, TAG_PROTOCOL = 4,
    TAG_CAPABILITIES = 5, TAG_TRANSPORT = 6, TAG_AUTH = 7, TAG_RESET = 8,
    TAG_PROJECT = 9, TAG_VERSION = 10, TAG_ELF = 11, TAG_IDF = 12,
    TAG_MAX_PAYLOAD = 13, TAG_ROLE = 14, TAG_CONTRACT = 15, TAG_CHIP = 16,
    TAG_BOARD = 17, TAG_ABI = 18, TAG_REQUIRED = 19, TAG_HEALTH_TIMEOUT = 20,
    TAG_LAYOUT = 21, TAG_MAC = 22, TAG_LINK_ROLE = 23, TAG_DATA_AVAILABLE = 25,
    TAG_LIFECYCLE = 0x29, TAG_CRASH_COUNT = 0x38, TAG_CRASH_ERROR = 0x3f,
};

typedef struct {
    bool bound;
    ev_link_ref_t link;
    char line[ACCEPT_LINE_SIZE];
    size_t length;
    bool discard;
    uint32_t line_started_ms;
    uint32_t wire_session;
    uint32_t tx_sequence[10];
    uint32_t rx_sequence[10];
    uint16_t rx_seen;
    bool active;
    bool auth_queued;
    uint8_t lifecycle_stage;
    char ev_session[33];
    char owner[33];
} acceptance_t;

static acceptance_t s_accept;
static uint32_t s_authenticated_wire_session;
typedef struct {
    char ev_session[33];
    uint32_t wire_session;
} auth_context_t;
_Static_assert(sizeof(auth_context_t) <= EV_MUX_TX_CONTEXT_MAX, "authentication completion context");
static const char base64_digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = (uint8_t)(value >> (i * 8));
    }
}

static void write64(uint8_t *p, uint64_t value)
{
    write32(p, (uint32_t)value);
    write32(p + 4, (uint32_t)(value >> 32));
}

static void hex_encode(const uint8_t *input, size_t size, char *output)
{
    const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        output[i * 2] = digits[input[i] >> 4];
        output[i * 2 + 1] = digits[input[i] & 15];
    }
    output[size * 2] = '\0';
}

static void hex_decode(const char *input, size_t size, uint8_t *output)
{
    for (size_t i = 0; i < size; ++i) {
        unsigned high = input[i * 2] <= '9' ? input[i * 2] - '0' : input[i * 2] - 'a' + 10;
        unsigned low = input[i * 2 + 1] <= '9' ? input[i * 2 + 1] - '0' : input[i * 2 + 1] - 'a' + 10;
        output[i] = (uint8_t)((high << 4) | low);
    }
}

static int base64_value(char c)
{
    if (c == '\0') {
        return -1;
    }
    const char *found = strchr(base64_digits, c);
    return found == NULL ? -1 : (int)(found - base64_digits);
}

static size_t base64_decode(const char *input, size_t size, uint8_t *output, size_t capacity)
{
    size_t length = 0;
    if (size == 0 || size % 4 != 0) {
        return 0;
    }
    for (size_t i = 0; i < size; i += 4) {
        int a = base64_value(input[i]), b = base64_value(input[i + 1]);
        bool pad2 = input[i + 2] == '=', pad3 = input[i + 3] == '=';
        int c = pad2 ? 0 : base64_value(input[i + 2]);
        int d = pad3 ? 0 : base64_value(input[i + 3]);
        size_t count = pad2 ? 1 : pad3 ? 2 : 3;
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad3) ||
                ((pad2 || pad3) && i + 4 != size) || (pad2 && (b & 15)) ||
                (pad3 && !pad2 && (c & 3)) || count > capacity - length) {
            return 0;
        }
        output[length++] = (uint8_t)((a << 2) | (b >> 4));
        if (count > 1) {
            output[length++] = (uint8_t)((b << 4) | (c >> 2));
        }
        if (count > 2) {
            output[length++] = (uint8_t)((c << 6) | d);
        }
    }
    return length;
}

static size_t base64_encode(const uint8_t *input, size_t size, char *output)
{
    size_t length = 0;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t word = (uint32_t)input[i] << 16;
        if (i + 1 < size) {
            word |= (uint32_t)input[i + 1] << 8;
        }
        if (i + 2 < size) {
            word |= input[i + 2];
        }
        output[length++] = base64_digits[word >> 18];
        output[length++] = base64_digits[(word >> 12) & 63];
        output[length++] = i + 1 < size ? base64_digits[(word >> 6) & 63] : '=';
        output[length++] = i + 2 < size ? base64_digits[word & 63] : '=';
    }
    return length;
}

static size_t cobs_decode(const uint8_t *input, size_t size, uint8_t *output, size_t capacity)
{
    size_t read = 0, written = 0;
    while (read < size) {
        unsigned code = input[read++];
        if (code == 0 || code - 1 > size - read || code - 1 > capacity - written) {
            return 0;
        }
        for (unsigned i = 1; i < code; ++i) {
            if (input[read] == 0) {
                return 0;
            }
            output[written++] = input[read++];
        }
        if (code < 255 && read < size) {
            if (written == capacity) {
                return 0;
            }
            output[written++] = 0;
        }
    }
    return written;
}

static size_t cobs_encode(const uint8_t *input, size_t size, uint8_t *output)
{
    size_t mark = 0, written = 1;
    unsigned code = 1;
    for (size_t i = 0; i < size; ++i) {
        if (input[i] == 0) {
            output[mark] = (uint8_t)code;
            mark = written++;
            code = 1;
        } else {
            output[written++] = input[i];
            if (++code == 255) {
                output[mark] = (uint8_t)code;
                mark = written++;
                code = 1;
            }
        }
    }
    output[mark] = (uint8_t)code;
    output[written++] = 0;
    return written;
}

static bool same_link(const ev_link_ref_t *a, const ev_link_ref_t *b)
{
    return a->transport_id == b->transport_id && a->slot == b->slot && a->generation == b->generation;
}

static bool owns(const ev_session_snapshot_t *snapshot)
{
    return s_accept.active && snapshot->has_owner && same_link(&snapshot->owner_link, &s_accept.link) &&
           strcmp(snapshot->session_id, s_accept.ev_session) == 0 &&
           strcmp(snapshot->client_nonce, s_accept.owner) == 0;
}

static void auth_complete(esp_err_t result, ev_link_ref_t link, const void *context, size_t length)
{
    if (length != sizeof(auth_context_t)) {
        return;
    }
    auth_context_t copied;
    memcpy(&copied, context, sizeof(copied));
    const auth_context_t *auth = &copied;
    if (result != ESP_OK) {
        (void)ev_session_hello_complete(&link, auth->ev_session, false);
        return;
    }
    ev_session_snapshot_t current;
    if (ev_session_snapshot(&current) && current.has_owner &&
            same_link(&current.owner_link, &link) && strcmp(current.session_id, auth->ev_session) == 0) {
        /* Reserve the common first-come lease, but never enable EV-MUX stream
         * output for an Iris client. Only its installer records become ready. */
        __atomic_store_n(&s_authenticated_wire_session, auth->wire_session, __ATOMIC_RELEASE);
    }
}

static bool send_frame(uint8_t channel, uint8_t type, uint32_t request,
                       const void *payload, size_t size, bool authentication)
{
    if (size > ACCEPT_PAYLOAD_MAX || channel >= 10) {
        return false;
    }
    uint8_t decoded[ACCEPT_HEADER_SIZE + ACCEPT_PAYLOAD_MAX + 4] = {0};
    uint8_t encoded[sizeof(decoded) + 4];
    char record[ACCEPT_LINE_SIZE];
    memcpy(decoded, "IRIS", 4);
    decoded[4] = 2;
    decoded[5] = ACCEPT_HEADER_SIZE;
    decoded[6] = channel;
    decoded[7] = type;
    write32(decoded + 12, s_accept.wire_session);
    write32(decoded + 16, request);
    write32(decoded + 24, s_accept.tx_sequence[channel]++);
    write32(decoded + 28, (uint32_t)size);
    if (size != 0) {
        memcpy(decoded + ACCEPT_HEADER_SIZE, payload, size);
    }
    write32(decoded + ACCEPT_HEADER_SIZE + size, ev_mux_crc32(decoded, ACCEPT_HEADER_SIZE + size));
    size_t bytes = cobs_encode(decoded, ACCEPT_HEADER_SIZE + size + 4, encoded);
    const char prefix[] = "\r\n@iris/0.2 ";
    memcpy(record, prefix, sizeof(prefix) - 1);
    size_t length = sizeof(prefix) - 1 + base64_encode(encoded, bytes, record + sizeof(prefix) - 1);
    record[length++] = '\r';
    record[length++] = '\n';
    auth_context_t auth = { .wire_session = s_accept.wire_session };
    memcpy(auth.ev_session, s_accept.ev_session, sizeof(auth.ev_session));
    ev_mux_submit_options_t options = {
        .link = &s_accept.link,
        .session_id = s_accept.active ? s_accept.ev_session : NULL,
        .tx_class = EV_MUX_TX_SESSION,
        .completion = authentication ? auth_complete : NULL,
        .completion_context = authentication ? &auth : NULL,
        .completion_context_len = authentication ? sizeof(auth) : 0,
    };
    return ev_mux_submit_installer_record(&options, record, length) == ESP_OK;
}

typedef struct {
    uint8_t data[ACCEPT_PAYLOAD_MAX];
    size_t length;
    bool failed;
} tlv_buffer_t;

static void tlv(tlv_buffer_t *buffer, uint8_t tag, const void *data, size_t size)
{
    if (buffer->failed || size + 3 > sizeof(buffer->data) - buffer->length) {
        buffer->failed = true;
        return;
    }
    uint8_t *out = buffer->data + buffer->length;
    out[0] = tag;
    out[1] = (uint8_t)size;
    out[2] = (uint8_t)(size >> 8);
    memcpy(out + 3, data, size);
    buffer->length += size + 3;
}

static void tlv_number(tlv_buffer_t *buffer, uint8_t tag, uint64_t value, size_t size)
{
    uint8_t encoded[8];
    write64(encoded, value);
    tlv(buffer, tag, encoded, size);
}

static void tlv_text(tlv_buffer_t *buffer, uint8_t tag, const char *text)
{
    tlv(buffer, tag, text, strlen(text));
}

static void boot_fields(tlv_buffer_t *buffer)
{
    uint8_t boot[8];
    hex_decode(esp_vision_firmware_boot_id(), sizeof(boot), boot);
    tlv(buffer, TAG_BOOT, boot, sizeof(boot));
    tlv_number(buffer, TAG_UPTIME, (uint64_t)esp_timer_get_time(), 8);
    tlv_number(buffer, TAG_RESET, esp_reset_reason(), 4);
}

static bool crash_reset(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT || reason == ESP_RST_PWR_GLITCH ||
           reason == ESP_RST_CPU_LOCKUP;
}

static bool healthy(void)
{
    return esp_vision_irisfw_boot_confirmed() && ev_session_vm_ready() && !crash_reset();
}

static void identity_fields(tlv_buffer_t *buffer)
{
    uint8_t device[16] = "ESP-IRIS\1";
    hex_decode(esp_vision_device_id(), 6, device + 10);
    tlv(buffer, TAG_DEVICE, device, sizeof(device));
    tlv(buffer, TAG_MAC, device + 10, 6);
    boot_fields(buffer);
    tlv_number(buffer, TAG_PROTOCOL, 2, 2);
    tlv_number(buffer, TAG_CAPABILITIES, (1ULL << 1) | (1ULL << 2) | (1ULL << 3) | (1ULL << 16), 8);
    tlv_number(buffer, TAG_TRANSPORT, 1, 1);
    tlv_number(buffer, TAG_AUTH, 0, 1);
    tlv_text(buffer, TAG_PROJECT, ESP_VISION_FIRMWARE_ID);
    tlv_text(buffer, TAG_VERSION, ESP_VISION_FIRMWARE_VERSION);
    const esp_app_desc_t *app = esp_app_get_description();
    tlv(buffer, TAG_ELF, app->app_elf_sha256, sizeof(app->app_elf_sha256));
    tlv_text(buffer, TAG_IDF, esp_get_idf_version());
    tlv_number(buffer, TAG_MAX_PAYLOAD, ACCEPT_PAYLOAD_MAX, 4);
    tlv_number(buffer, TAG_ROLE, 1, 1);
    tlv_text(buffer, TAG_CONTRACT, "esp-mosaico/v2");
    tlv_text(buffer, TAG_CHIP, "esp32s31");
    tlv_text(buffer, TAG_BOARD, "esp-mosaico");
    tlv_text(buffer, TAG_LAYOUT, "mosaico-retained-test-2m-v2");
    tlv_number(buffer, TAG_ABI, 2, 2);
    tlv_number(buffer, TAG_REQUIRED, 0, 8);
    tlv_number(buffer, TAG_HEALTH_TIMEOUT, 45000, 4);
    tlv_number(buffer, TAG_LINK_ROLE, 0, 1);
    tlv_number(buffer, TAG_DATA_AVAILABLE, 0, 1);
}

static void send_error(uint8_t channel, uint8_t type, uint32_t request, esp_err_t error)
{
    uint8_t payload[8] = {0};
    write32(payload, (uint32_t)error);
    payload[4] = channel;
    payload[5] = type;
    (void)send_frame(CH_CONTROL, CT_ERROR, request, payload, sizeof(payload), false);
}

void esp_vision_irisfw_acceptance_poll(uint32_t now_ms)
{
    (void)now_ms;
    if (!s_accept.active) {
        return;
    }
    ev_session_snapshot_t current;
    if (!ev_session_snapshot(&current) || !owns(&current)) {
        memset(&s_accept, 0, sizeof(s_accept));
        return;
    }
    if (!s_accept.auth_queued) {
        const uint8_t accepted = 1;
        s_accept.auth_queued = send_frame(CH_CONTROL, CT_AUTH, 0, &accepted, 1, true);
        return;
    }
    if (__atomic_load_n(&s_authenticated_wire_session, __ATOMIC_ACQUIRE) != s_accept.wire_session ||
            s_accept.lifecycle_stage >= 3) {
        return;
    }
    uint8_t event = s_accept.lifecycle_stage == 0 ? EV_BOOT :
                    s_accept.lifecycle_stage == 1 ? EV_LINK_READY : EV_HEALTHY;
    if (event == EV_HEALTHY && !healthy()) {
        return;
    }
    tlv_buffer_t buffer = {0};
    boot_fields(&buffer);
    if (send_frame(CH_EVENT, event, 0, buffer.data, buffer.length, false)) {
        ++s_accept.lifecycle_stage;
    }
}

static void receive_frame(const uint8_t *frame, size_t length, uint32_t now_ms)
{
    if (length < 36 || memcmp(frame, "IRIS", 4) != 0 || frame[4] != 2 ||
            frame[5] != 32 || frame[6] >= 10 || read32(frame + 8) != 0 ||
            read32(frame + 20) != 0 || read32(frame + 12) != s_accept.wire_session ||
            read32(frame + 28) != length - 36 ||
            read32(frame + length - 4) != ev_mux_crc32(frame, length - 4)) {
        return;
    }
    uint8_t channel = frame[6], type = frame[7];
    uint32_t request = read32(frame + 16), sequence = read32(frame + 24);
    if ((s_accept.rx_seen & (1U << channel)) &&
            (int32_t)(sequence - s_accept.rx_sequence[channel]) <= 0) {
        return;
    }
    s_accept.rx_seen |= (uint16_t)(1U << channel);
    s_accept.rx_sequence[channel] = sequence;
    const uint8_t *payload = frame + 32;
    size_t size = length - 36;
    if (channel == CH_CONTROL && type == CT_ACK) {
        if (size != 17 || payload[0] != 0 || request != 0) {
            return;
        }
        char owner[33];
        hex_encode(payload + 1, 16, owner);
        ev_session_snapshot_t current;
        if (!ev_session_snapshot(&current) ||
                (current.has_owner && (!owns(&current) || strcmp(owner, s_accept.owner) != 0))) {
            return;
        }
        if (strcmp(owner, "00000000000000000000000000000000") == 0) {
            const uint8_t rejected = 0;
            (void)send_frame(CH_CONTROL, CT_AUTH, 0, &rejected, 1, false);
            return;
        }
        if (!s_accept.active &&
                ev_session_acquire(&s_accept.link, owner, now_ms, &current) != EV_SESSION_RESULT_OK) {
            return;
        }
        if (ev_session_pending_keepalive(&s_accept.link, current.session_id, now_ms, &current) != EV_SESSION_RESULT_OK) {
            return;
        }
        memcpy(s_accept.owner, owner, sizeof(owner));
        memcpy(s_accept.ev_session, current.session_id, sizeof(s_accept.ev_session));
        s_accept.active = true;
        s_accept.auth_queued = false;
        esp_vision_irisfw_acceptance_poll(now_ms);
        return;
    }
    if (!s_accept.active) {
        return;
    }
    ev_session_snapshot_t current;
    if (ev_session_pending_keepalive(&s_accept.link, s_accept.ev_session, now_ms, &current) != EV_SESSION_RESULT_OK ||
            !owns(&current) ||
            __atomic_load_n(&s_authenticated_wire_session, __ATOMIC_ACQUIRE) != s_accept.wire_session) {
        return;
    }
    if (channel == CH_CONTROL && type == CT_CREDIT && size == 8) {
        return;
    }
    if (request == 0) {
        return;
    }
    if (channel == CH_CONTROL && type == CT_PING) {
        (void)send_frame(CH_CONTROL, CT_PONG, request, payload, size, false);
    } else if (channel == CH_CONTROL && type == CT_TIME && size == 8) {
        uint8_t response[24];
        memcpy(response, payload, 8);
        write64(response + 8, (uint64_t)esp_timer_get_time());
        write64(response + 16, (uint64_t)esp_timer_get_time());
        (void)send_frame(CH_CONTROL, CT_TIME_REPLY, request, response, sizeof(response), false);
    } else if (channel == CH_CONTROL && type == CT_STATUS && size == 0) {
        tlv_buffer_t buffer = {0};
        boot_fields(&buffer);
        tlv_number(&buffer, TAG_LIFECYCLE, healthy() ? 2 : 1, 1);
        tlv_number(&buffer, TAG_CRASH_COUNT, crash_reset() ? 1 : 0, 4);
        tlv_number(&buffer, TAG_CRASH_ERROR, healthy() ? 0 : ESP_ERR_INVALID_STATE, 4);
        (void)send_frame(CH_CONTROL, CT_STATUS_REPLY, request, buffer.data, buffer.length, false);
    } else if (channel == CH_UPDATE && type == UPDATE_INVENTORY && size == 0) {
        esp_vision_firmware_status_t inventory;
        esp_err_t result = esp_vision_firmware_inventory(&inventory);
        if (result != ESP_OK) {
            send_error(channel, type, request, result);
            return;
        }
        uint8_t response[92] = {0};
        write32(response, 7);
        write32(response + 4, 5);
        hex_decode(inventory.bootloader_sha256, 32, response + 8);
        hex_decode(inventory.partition_table_sha256, 32, response + 40);
        hex_decode(inventory.operation_id, 16, response + 72);
        write32(response + 88, (uint32_t)inventory.commit_result);
        (void)send_frame(CH_UPDATE, UPDATE_INVENTORY_REPLY, request, response, sizeof(response), false);
    } else {
        send_error(channel, type, request, ESP_ERR_NOT_SUPPORTED);
    }
}

static void receive_line(uint32_t now_ms)
{
    if (strcmp(s_accept.line, "iris hello") == 0) {
        ev_session_snapshot_t current;
        if (!ev_session_snapshot(&current) || (current.has_owner && !owns(&current))) {
            return;
        }
        /* Native console clients probe HELLO more frequently than their clock
         * requests. Keep only the already-bound USB lease alive on this probe. */
        if (owns(&current) &&
                ev_session_pending_keepalive(&s_accept.link, s_accept.ev_session, now_ms, &current) != EV_SESSION_RESULT_OK) {
            return;
        }
        if (s_accept.wire_session == 0) {
            do {
                esp_fill_random(&s_accept.wire_session, sizeof(s_accept.wire_session));
            } while (s_accept.wire_session == 0);
        }
        tlv_buffer_t buffer = {0};
        identity_fields(&buffer);
        if (!buffer.failed) {
            (void)send_frame(CH_CONTROL, CT_HELLO, 0, buffer.data, buffer.length, false);
        }
    } else if (strncmp(s_accept.line, "iris @", 6) == 0 && s_accept.wire_session != 0) {
        uint8_t encoded[ACCEPT_HEADER_SIZE + ACCEPT_PAYLOAD_MAX + 8];
        uint8_t decoded[ACCEPT_HEADER_SIZE + ACCEPT_PAYLOAD_MAX + 4];
        size_t length = base64_decode(s_accept.line + 6, s_accept.length - 6, encoded, sizeof(encoded));
        if (length < 2 || encoded[length - 1] != 0) {
            return;
        }
        size_t size = cobs_decode(encoded, length - 1, decoded, sizeof(decoded));
        if (size != 0) {
            receive_frame(decoded, size, now_ms);
        }
    }
}

void esp_vision_irisfw_acceptance_frame_start(void)
{
    s_accept.length = 0;
    s_accept.discard = false;
}

void esp_vision_irisfw_acceptance_byte(const ev_link_ref_t *link, uint8_t byte, uint32_t now_ms)
{
    if (link == NULL || link->transport_id != EV_TRANSPORT_USB) {
        return;
    }
    if (!s_accept.bound || !same_link(link, &s_accept.link)) {
        memset(&s_accept, 0, sizeof(s_accept));
        s_accept.bound = true;
        s_accept.link = *link;
    }
    if (s_accept.length != 0 && (uint32_t)(now_ms - s_accept.line_started_ms) > 3000) {
        s_accept.discard = true;
    }
    if (byte == '\r' || byte == '\n') {
        if (!s_accept.discard && s_accept.length != 0) {
            s_accept.line[s_accept.length] = '\0';
            receive_line(now_ms);
        }
        s_accept.length = 0;
        s_accept.discard = false;
        return;
    }
    if (byte < 32 || byte > 126 || s_accept.length + 1 >= sizeof(s_accept.line)) {
        s_accept.discard = true;
    }
    if (!s_accept.discard) {
        if (s_accept.length == 0) {
            s_accept.line_started_ms = now_ms;
        }
        s_accept.line[s_accept.length++] = (char)byte;
    }
}

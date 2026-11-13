/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../boards/ESP32_S31_MOSAICO/irisfw_acceptance.c"

static ev_session_snapshot_t mock_session;
static ev_link_ref_t mock_link = { .transport_id = EV_TRANSPORT_USB, .slot = 0, .generation = 1 };
static bool confirmed = true, ready = true, wire_output;
static esp_reset_reason_t reset_reason = ESP_RST_SW;
static int inventory_error, submit_failures;
static unsigned acquired, pending_activity, completed_ev_handshakes;
static uint32_t random_value = 1234;
static uint32_t now_ms = 100;
static char output[32768];
static size_t output_length;

int64_t esp_timer_get_time(void)
{
    return (int64_t)now_ms * 1000;
}
esp_reset_reason_t esp_reset_reason(void)
{
    return reset_reason;
}
const char *esp_get_idf_version(void)
{
    return "v6.1-test";
}
const char *esp_vision_device_id(void)
{
    return "001122334455";
}
const char *esp_vision_firmware_boot_id(void)
{
    return "0102030405060708090a0b0c0d0e0f10";
}
bool esp_vision_irisfw_boot_confirmed(void)
{
    return confirmed;
}
bool ev_session_vm_ready(void)
{
    return ready;
}
void esp_fill_random(void *data, size_t size)
{
    assert(size == 4);
    uint32_t value = ++random_value;
    memcpy(data, &value, sizeof(value));
}
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t description = { .app_elf_sha256 = { 0x12, 0x34 } };
    return &description;
}
bool ev_session_snapshot(ev_session_snapshot_t *snapshot)
{
    *snapshot = mock_session;
    snapshot->vm_ready = ready;
    return true;
}
ev_session_result_t ev_session_acquire(const ev_link_ref_t *link, const char *nonce,
                                       uint32_t time_ms, ev_session_snapshot_t *snapshot)
{
    (void)time_ms;
    if (mock_session.has_owner && (!same_link(link, &mock_session.owner_link) ||
                                   strcmp(nonce, mock_session.client_nonce) != 0)) {
        return EV_SESSION_RESULT_BUSY;
    }
    if (!mock_session.has_owner) {
        ++acquired;
        mock_session.state = EV_SESSION_OWNED;
        mock_session.has_owner = true;
        mock_session.owner_link = *link;
        memcpy(mock_session.client_nonce, nonce, 33);
        memcpy(mock_session.session_id, "112233445566778899aabbccddeeff00", 33);
    }
    *snapshot = mock_session;
    return EV_SESSION_RESULT_OK;
}
ev_session_result_t ev_session_pending_keepalive(const ev_link_ref_t *link, const char *session,
                                                 uint32_t time_ms, ev_session_snapshot_t *snapshot)
{
    (void)time_ms;
    if (!mock_session.has_owner || mock_session.handshake_complete ||
            !same_link(link, &mock_session.owner_link) || strcmp(session, mock_session.session_id) != 0) {
        return EV_SESSION_RESULT_SESSION_REQUIRED;
    }
    ++pending_activity;
    mock_session.installer_owner = true;
    *snapshot = mock_session;
    return EV_SESSION_RESULT_OK;
}
bool ev_session_hello_complete(const ev_link_ref_t *link, const char *session, bool succeeded)
{
    if (!mock_session.has_owner || !same_link(link, &mock_session.owner_link) ||
            strcmp(session, mock_session.session_id) != 0) {
        return false;
    }
    if (succeeded) {
        ++completed_ev_handshakes;
        mock_session.handshake_complete = true;
    } else {
        memset(&mock_session, 0, sizeof(mock_session));
    }
    return true;
}
uint32_t ev_mux_crc32(const void *data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}
esp_err_t ev_mux_submit_installer_record(const ev_mux_submit_options_t *options,
                                         const void *data, size_t size)
{
    assert(options->link->transport_id == EV_TRANSPORT_USB && options->tx_class == EV_MUX_TX_SESSION);
    assert(size <= 1024 && size > 0);
    if (submit_failures > 0) {
        --submit_failures;
        return ESP_FAIL;
    }
    assert(output_length + size < sizeof(output));
    memcpy(output + output_length, data, size);
    output_length += size;
    output[output_length] = '\0';
    if (wire_output) {
        assert(fwrite(data, 1, size, stdout) == size);
        fflush(stdout);
        output_length = 0;
    }
    if (options->completion != NULL) {
        uint8_t unaligned[EV_MUX_TX_CONTEXT_MAX + 1];
        memcpy(unaligned + 1, options->completion_context, options->completion_context_len);
        options->completion(ESP_OK, *options->link, unaligned + 1, options->completion_context_len);
    }
    return ESP_OK;
}
esp_err_t esp_vision_firmware_inventory(esp_vision_firmware_status_t *inventory)
{
    if (inventory_error) {
        return inventory_error;
    }
    memset(inventory, 0, sizeof(*inventory));
    memset(inventory->bootloader_sha256, 'a', 64);
    memset(inventory->partition_table_sha256, 'b', 64);
    strcpy(inventory->operation_id, "0102030405060708090a0b0c0d0e0f10");
    return ESP_OK;
}

static void clear_output(void)
{
    output_length = 0;
    output[0] = '\0';
}
static void poll(void)
{
    for (unsigned i = 0; i < 5; ++i) {
        esp_vision_irisfw_acceptance_poll(now_ms);
    }
}
static void input(const char *text)
{
    while (*text != '\0') {
        esp_vision_irisfw_acceptance_byte(&mock_link, (uint8_t)*text++, now_ms);
    }
    poll();
}
static size_t read_record(size_t *cursor, uint8_t *frame)
{
    const char *start = strstr(output + *cursor, "@iris/0.2 ");
    if (start == NULL) {
        return 0;
    }
    start += 10;
    const char *end = strchr(start, '\r');
    assert(end != NULL);
    uint8_t encoded[560];
    size_t length = base64_decode(start, (size_t)(end - start), encoded, sizeof(encoded));
    assert(length > 0 && encoded[length - 1] == 0);
    size_t size = cobs_decode(encoded, length - 1, frame, 548);
    assert(size >= 36 && memcmp(frame, "IRIS\2\40", 6) == 0);
    assert(read32(frame + size - 4) == ev_mux_crc32(frame, size - 4));
    *cursor = (size_t)(end - output) + 2;
    return size;
}
static unsigned event_count(unsigned event)
{
    size_t cursor = 0, length;
    unsigned count = 0;
    uint8_t frame[548];
    while ((length = read_record(&cursor, frame)) != 0) {
        (void)length;
        if (frame[6] == CH_EVENT && frame[7] == event) {
            ++count;
        }
    }
    return count;
}
static void request(unsigned channel, unsigned type, unsigned sequence, unsigned id,
                    const uint8_t *payload, size_t size, bool corrupt)
{
    uint8_t frame[548] = "IRIS", encoded[552];
    char line[768] = "iris @";
    frame[4] = 2; frame[5] = 32; frame[6] = channel; frame[7] = type;
    write32(frame + 12, s_accept.wire_session);
    write32(frame + 16, id);
    write32(frame + 24, sequence);
    write32(frame + 28, size);
    if (size) {
        memcpy(frame + 32, payload, size);
    }
    write32(frame + 32 + size, ev_mux_crc32(frame, 32 + size) ^ (corrupt ? 1 : 0));
    size_t length = cobs_encode(frame, size + 36, encoded);
    size_t text_length = 6 + base64_encode(encoded, length, line + 6);
    line[text_length++] = '\n'; line[text_length] = '\0';
    input(line);
}
static void hello_and_ack(void)
{
    input("iris hello\r\n");
    uint8_t owner[17] = {0};
    for (unsigned i = 1; i < sizeof(owner); ++i) {
        owner[i] = i;
    }
    request(CH_CONTROL, CT_ACK, 0, 0, owner, sizeof(owner), false);
}
static void assert_response(unsigned channel, unsigned type, unsigned id)
{
    size_t cursor = 0;
    uint8_t frame[548];
    assert(read_record(&cursor, frame) != 0);
    assert(frame[6] == channel && frame[7] == type && read32(frame + 16) == id);
    assert(read_record(&cursor, frame) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    if (strcmp(scenario, "wire") == 0) {
        wire_output = true;
        int byte;
        while ((byte = getchar()) != EOF) {
            esp_vision_irisfw_acceptance_byte(&mock_link, (uint8_t)byte, now_ms++);
            poll();
        }
        return 0;
    }
    if (strcmp(scenario, "foreign-owner") == 0) {
        input("iris hello\n");
        clear_output();
        mock_session.has_owner = true;
        mock_session.handshake_complete = true;
        strcpy(mock_session.session_id, "ffffffffffffffffffffffffffffffff");
        hello_and_ack();
        assert(output_length == 0 && acquired == 0 && mock_session.handshake_complete);
        return 0;
    }
    if (strcmp(scenario, "framing") == 0) {
        input("iris he");
        esp_vision_irisfw_acceptance_frame_start();
        input("llo\n");
        assert(output_length == 0);
        char long_line[900];
        memset(long_line, 'x', sizeof(long_line) - 1);
        long_line[sizeof(long_line) - 1] = '\0';
        input(long_line);
        input("iris hello\n");
        assert(output_length == 0);
        input("iris hello\n");
        assert(output_length != 0);
        clear_output();
        input("iris hello");
        now_ms += 3001;
        input("\n");
        assert(output_length == 0);
        return 0;
    }
    if (strcmp(scenario, "not-ready") == 0) {
        confirmed = false;
    }
    if (strcmp(scenario, "crash") == 0) {
        reset_reason = ESP_RST_PANIC;
    }
    if (strcmp(scenario, "tx-retry") == 0) {
        input("iris hello\n");
        submit_failures = 2;
    }
    hello_and_ack();
    assert(acquired == 1 && mock_session.has_owner && !mock_session.handshake_complete);
    assert(completed_ev_handshakes == 0);
    assert(mock_session.installer_owner);
    assert(event_count(EV_BOOT) == 1 && event_count(EV_LINK_READY) == 1);
    assert(event_count(EV_HEALTHY) == (confirmed && !crash_reset() ? 1U : 0U));
    if (!confirmed) {
        confirmed = true;
        poll();
        assert(event_count(EV_HEALTHY) == 1);
    }
    poll();
    assert(event_count(EV_HEALTHY) <= 1);
    clear_output();
    pending_activity = 0;
    request(CH_CONTROL, CT_STATUS, 1, 100, NULL, 0, false);
    assert_response(CH_CONTROL, CT_STATUS_REPLY, 100);
    assert(pending_activity == 1);
    clear_output();
    request(CH_CONTROL, CT_STATUS, 1, 101, NULL, 0, false);
    assert(output_length == 0); // duplicate sequence cannot refresh the lease
    request(CH_CONTROL, CT_STATUS, 2, 102, NULL, 0, true);
    assert(output_length == 0 && pending_activity == 1);
    input("iris @AB==\n");
    assert(output_length == 0 && pending_activity == 1);
    request(CH_CONTROL, 0x15, 2, 103, (const uint8_t *)"\0\0\0\0", 4, false);
    assert_response(CH_CONTROL, CT_ERROR, 103); // restart and update entry are not implemented
    clear_output();
    request(CH_UPDATE, UPDATE_INVENTORY, 0, 104, NULL, 0, false);
    assert_response(CH_UPDATE, UPDATE_INVENTORY_REPLY, 104);
    clear_output();
    inventory_error = ESP_FAIL;
    request(CH_UPDATE, UPDATE_INVENTORY, 1, 105, NULL, 0, false);
    assert_response(CH_CONTROL, CT_ERROR, 105);
    clear_output();
    memset(&mock_session, 0, sizeof(mock_session)); // disconnect/lease loss
    poll();
    assert(!s_accept.active && output_length == 0);
    puts("installer acceptance isolation passed");
    return 0;
}

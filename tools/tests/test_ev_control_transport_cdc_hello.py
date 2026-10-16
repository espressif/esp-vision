#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Check CDC mount emits no protocol event and hello requests stay correlated."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CONTROL_SOURCE = ROOT / "platform/ev_control_transport.c"


class CdcHelloContractTests(unittest.TestCase):
    def test_mount_has_no_unsolicited_event_and_hello_reply_keeps_request_seq(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        production = CONTROL_SOURCE.read_text(encoding="utf-8")
        open_start = production.index("static void ev_control_transport_cdc_open(void)")
        open_end = production.index("static void ev_control_transport_cdc_update(void)", open_start)
        cdc_open = production[open_start:open_end]
        self.assertNotIn("ev_control_send_hello", cdc_open)
        self.assertNotIn("ev_control_send_rpc", cdc_open)
        self.assertIn("ev_transport_link_advance_generation", cdc_open)
        self.assertIn("ev_control_transport_cdc_reset_parser", cdc_open)

        acquire_start = production.index("static esp_err_t ev_control_send_hello_acquire(")
        acquire_end = production.index("static esp_err_t ev_control_send_transport_state(", acquire_start)
        acquire_handler = production[acquire_start:acquire_end]
        self.assertIn("return ev_control_send_hello_discovery(seq);", acquire_handler)

        start_marker = "static esp_err_t ev_control_build_hello_discovery_payload("
        end_marker = "static esp_err_t ev_control_send_hello_acquire("
        helpers = production[production.index(start_marker):production.index(end_marker)]

        prelude = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_INVALID_SIZE -3

typedef enum {
    EV_TRANSPORT_USJ = 0,
    EV_TRANSPORT_USB,
    EV_TRANSPORT_UART,
    EV_TRANSPORT_TCP,
} ev_transport_id_t;
typedef struct {
    bool has_owner;
    bool vm_ready;
    struct { ev_transport_id_t transport_id; uint8_t slot; uint32_t generation; } owner_link;
} ev_session_snapshot_t;

static ev_session_snapshot_t s_session;
static char s_rpc_type[16];
static char s_rpc_method[40];
static char s_rpc_payload[768];
static uint32_t s_rpc_seq;

static const char *ev_control_bool_str(bool value)
{
    return value ? "true" : "false";
}

static bool ev_session_snapshot(ev_session_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }
    *snapshot = s_session;
    return true;
}

static const char *ev_transport_name(ev_transport_id_t id)
{
    switch (id) {
    case EV_TRANSPORT_USJ: return "usj";
    case EV_TRANSPORT_USB: return "usb";
    case EV_TRANSPORT_UART: return "uart";
    case EV_TRANSPORT_TCP: return "tcp";
    default: return "unknown";
    }
}

static const char *esp_get_idf_version(void)
{
    return "test-idf";
}

static esp_err_t ev_control_send_rpc_with_seq(const char *type,
                                               const char *method,
                                               const char *payload,
                                               uint32_t seq)
{
    snprintf(s_rpc_type, sizeof(s_rpc_type), "%s", type);
    snprintf(s_rpc_method, sizeof(s_rpc_method), "%s", method);
    snprintf(s_rpc_payload, sizeof(s_rpc_payload), "%s", payload);
    s_rpc_seq = seq;
    return ESP_OK;
}

static esp_err_t ev_control_send_error(const char *method,
                                        uint32_t seq,
                                        const char *code,
                                        const char *message)
{
    (void)method;
    (void)seq;
    (void)code;
    (void)message;
    return ESP_ERR_INVALID_STATE;
}

static const char *esp_vision_device_id(void) { return "aabbcc123456"; }
static const char *esp_vision_hostname(void) { return "esp-vision-test-board-123456-test-version"; }

static const char ESP_VISION_FIRMWARE_ID[] = "test-device";
static const char ESP_VISION_FIRMWARE_VERSION[] = "test-version";
static const char ESP_VISION_BOARD_TYPE[] = "test-board";
static const char ESP_VISION_BOARD_ARCH[] = "test-arch";

'''
        main = r'''
int main(void)
{
    s_session.has_owner = true;
    s_session.vm_ready = false;
    s_session.owner_link.transport_id = EV_TRANSPORT_TCP;
    assert(ev_control_send_hello_discovery(37) == ESP_OK);
    assert(strcmp(s_rpc_type, "rsp") == 0);
    assert(strcmp(s_rpc_method, "hello") == 0);
    assert(s_rpc_seq == 37);
    assert(strstr(s_rpc_payload, "\"evMuxVersion\":4") != NULL);
    assert(strstr(s_rpc_payload, "\"hostname\":\"esp-vision-test-board-123456-test-version\"") != NULL);
    assert(strstr(s_rpc_payload, "\"deviceId\":\"aabbcc123456\"") != NULL);
    assert(strstr(s_rpc_payload, "\"owner\":\"tcp\"") != NULL);
    assert(strstr(s_rpc_payload, "\"vmReady\":false") != NULL);

    s_session.has_owner = false;
    s_session.vm_ready = true;
    assert(ev_control_send_hello_discovery(38) == ESP_OK);
    assert(strcmp(s_rpc_type, "rsp") == 0);
    assert(strcmp(s_rpc_method, "hello") == 0);
    assert(s_rpc_seq == 38);
    assert(strstr(s_rpc_payload, "\"session\":null") != NULL);
    assert(strstr(s_rpc_payload, "\"vmReady\":true") != NULL);

    s_session.has_owner = true;
    s_session.owner_link.transport_id = EV_TRANSPORT_USB;
    assert(ev_control_send_hello_discovery(39) == ESP_OK);
    assert(s_rpc_seq == 39);
    assert(strstr(s_rpc_payload, "\"owner\":\"usb\"") != NULL);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="evmux-cdc-hello-") as directory:
            source = Path(directory) / "cdc_hello.c"
            binary = Path(directory) / "cdc_hello"
            source.write_text(prelude + helpers + main, encoding="utf-8")
            subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(source), "-o", str(binary),
            ], check=True, cwd=ROOT, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()

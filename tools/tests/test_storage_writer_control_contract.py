#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile the production EV-MUX storage-writer RPC formatter against fakes."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class StorageWriterControlContractTests(unittest.TestCase):
    def test_ev_mux_session_writer_status_acquire_release_contract(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        production = (ROOT / "platform/ev_control_transport.c").read_text(encoding="utf-8")
        start = production.index("static esp_err_t ev_control_send_storage_writer_control(")
        end = production.index("\nstatic esp_err_t ev_control_send_device_control", start)
        formatter = production[start:end]
        harness = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "storage_writer.h"

static char s_response_session_id[33] = "0123456789abcdef0123456789abcdef";
static char response[1024];
static esp_vision_storage_writer_mode_t writer_mode = ESP_VISION_STORAGE_WRITER_AVAILABLE;
static char active_operation[33];
static char active_lease[33];
static char active_session[33];
static bool writer_writable;
static bool flash_present = true;
static bool flash_mounted = true;
static bool sd_present = true;
static bool sd_mounted;

static bool ev_control_json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[48];
    int count = snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    if (count <= 0 || (size_t)count >= sizeof(pattern)) return false;
    const char *value = strstr(json, pattern);
    if (!value) return false;
    value += (size_t)count;
    const char *end = strchr(value, '\"');
    if (!end || (size_t)(end - value) >= out_len) return false;
    memcpy(out, value, (size_t)(end - value));
    out[end - value] = '\0';
    return true;
}

static bool ev_control_json_get_bool(const char *json, const char *key, bool fallback)
{
    char pattern[48];
    int count = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (count <= 0 || (size_t)count >= sizeof(pattern)) return fallback;
    const char *value = strstr(json, pattern);
    if (!value) return fallback;
    value += (size_t)count;
    if (strncmp(value, "true", 4) == 0) return true;
    if (strncmp(value, "false", 5) == 0) return false;
    return fallback;
}

static const char *ev_control_bool_str(bool value) { return value ? "true" : "false"; }

bool esp_vision_storage_flash_is_present(void) { return flash_present; }
bool esp_vision_storage_flash_is_mounted(void) { return flash_mounted; }
bool esp_vision_sdcard_is_present(void) { return sd_present; }
bool esp_vision_storage_sdcard_is_mounted(void) { return sd_mounted; }

static esp_err_t ev_control_send_error(const char *method, uint32_t seq, const char *code, const char *message)
{
    (void)method; (void)seq; (void)message;
    snprintf(response, sizeof(response), "{\"ok\":false,\"code\":\"%s\"}", code);
    return ESP_OK;
}

static esp_err_t ev_control_send_rpc_with_seq(const char *type, const char *method, const char *payload, uint32_t seq)
{
    (void)type; (void)method; (void)seq;
    snprintf(response, sizeof(response), "%s", payload);
    return ESP_OK;
}

esp_err_t esp_vision_msc_writer_status(esp_vision_storage_volume_t volume, esp_vision_storage_writer_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->mode = writer_mode;
    out->writable = writer_writable;
    out->reason = writer_writable ? "HOST_WRITER_ACTIVE" : NULL;
    if (writer_mode == ESP_VISION_STORAGE_WRITER_HELD) {
        memcpy(out->operation_id, active_operation, sizeof(out->operation_id));
        memcpy(out->lease_id, active_lease, sizeof(out->lease_id));
        memcpy(out->session_id, active_session, sizeof(out->session_id));
    }
    return volume == ESP_VISION_STORAGE_VOLUME_FLASH ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_vision_msc_writer_acquire(esp_vision_storage_volume_t volume, const char *operation_id,
                                       const char *session_id, char out_lease_id[33])
{
    if (volume != ESP_VISION_STORAGE_VOLUME_FLASH || strlen(operation_id) != 32 || strlen(session_id) != 32 ||
            writer_mode != ESP_VISION_STORAGE_WRITER_AVAILABLE || writer_writable) return ESP_ERR_INVALID_STATE;
    snprintf(active_operation, sizeof(active_operation), "%s", operation_id);
    snprintf(active_session, sizeof(active_session), "%s", session_id);
    snprintf(active_lease, sizeof(active_lease), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    snprintf(out_lease_id, 33, "%s", active_lease);
    writer_mode = ESP_VISION_STORAGE_WRITER_HELD;
    writer_writable = true;
    return ESP_OK;
}

esp_err_t esp_vision_msc_writer_release(esp_vision_storage_volume_t volume, const char *operation_id,
                                       const char *lease_id, const char *session_id, bool safe_handoff)
{
    if (volume != ESP_VISION_STORAGE_VOLUME_FLASH || !safe_handoff || strcmp(operation_id, active_operation) ||
            strcmp(lease_id, active_lease) || strcmp(session_id, active_session)) return ESP_ERR_INVALID_STATE;
    writer_mode = ESP_VISION_STORAGE_WRITER_HANDOFF_READY;
    writer_writable = false;
    active_operation[0] = active_lease[0] = active_session[0] = '\0';
    return ESP_OK;
}

'''
        harness += formatter
        harness += r'''
int main(void)
{
    const char *op = "11111111111111111111111111111111";
    assert(ev_control_send_storage_writer_control(1, "{\"volume\":\"flash\"}", "storage.writer.status") == ESP_OK);
    assert(strstr(response, "\"status\":\"available\""));
    assert(strstr(response, "\"writable\":false"));
    assert(strstr(response, "\"present\":true"));
    assert(strstr(response, "\"mounted\":true"));
    assert(strstr(response, "\"lease_id\":null"));

    char request[256];
    snprintf(request, sizeof(request), "{\"volume\":\"flash\",\"operationId\":\"%s\"}", op);
    assert(ev_control_send_storage_writer_control(2, request, "storage.writer.acquire") == ESP_OK);
    assert(strstr(response, "\"status\":\"held\"") == NULL);
    assert(strstr(response, "\"operation_id\":\"11111111111111111111111111111111\""));
    assert(strstr(response, "\"lease_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\""));
    assert(strstr(response, "\"session_id\":\"0123456789abcdef0123456789abcdef\""));

    assert(ev_control_send_storage_writer_control(3, "{\"volume\":\"flash\"}", "storage.writer.status") == ESP_OK);
    assert(strstr(response, "\"status\":\"held\""));
    assert(strstr(response, "\"writable\":true"));

    snprintf(request, sizeof(request), "{\"volume\":\"flash\",\"operationId\":\"%s\","
             "\"leaseId\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"safeHandoff\":true}", op);
    assert(ev_control_send_storage_writer_control(4, request, "storage.writer.release") == ESP_OK);
    assert(strstr(response, "\"status\":\"handoff_ready\""));
    assert(strstr(response, "\"writable\":false"));

    /* SD is physically present but not mounted, so it is not absent. */
    assert(ev_control_send_storage_writer_control(7, "{\"volume\":\"sd\"}", "storage.writer.status") == ESP_OK);
    assert(strstr(response, "\"status\":\"unknown\""));
    assert(strstr(response, "\"present\":true"));
    assert(strstr(response, "\"mounted\":false"));
    sd_present = false;
    assert(ev_control_send_storage_writer_control(8, "{\"volume\":\"sd\"}", "storage.writer.status") == ESP_OK);
    assert(strstr(response, "\"status\":\"unknown\""));
    assert(strstr(response, "\"present\":false"));
    assert(strstr(response, "\"mounted\":false"));

    /* Unmounted Flash with its partition present is unknown, not absent. */
    flash_mounted = false;
    assert(ev_control_send_storage_writer_control(9, "{\"volume\":\"flash\"}", "storage.writer.status") == ESP_OK);
    assert(strstr(response, "\"present\":true"));
    assert(strstr(response, "\"mounted\":false"));
    flash_present = false;
    assert(ev_control_send_storage_writer_control(10, "{\"volume\":\"flash\"}", "storage.writer.status") == ESP_OK);
    assert(strstr(response, "\"present\":false"));
    assert(strstr(response, "\"mounted\":false"));

    assert(ev_control_send_storage_writer_control(11, "{\"volume\":\"root\"}", "storage.writer.status") == ESP_OK);
    assert(strstr(response, "\"code\":\"INVALID_ARGUMENT\""));
    s_response_session_id[0] = '\0';
    assert(ev_control_send_storage_writer_control(12, request, "storage.writer.acquire") == ESP_OK);
    assert(strstr(response, "\"code\":\"SESSION_REQUIRED\""));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ev-storage-writer-control-") as directory:
            source = Path(directory) / "writer_control.c"
            binary = Path(directory) / "writer_control"
            source.write_text(harness, encoding="utf-8")
            subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'tools/tests/storage_writer_service_stubs'}",
                f"-I{ROOT / 'platform'}", str(source), "-o", str(binary),
            ], check=True, capture_output=True, text=True, cwd=ROOT)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True, cwd=ROOT)

        self.assertIn('return ev_control_send_storage_writer_control(seq, request, action);', production)
        self.assertIn('esp_vision_msc_writer_session_changed(snapshot->has_owner ? snapshot->session_id : NULL);', production)
        self.assertIn('storage.writer.lease', production)


if __name__ == "__main__":
    unittest.main()

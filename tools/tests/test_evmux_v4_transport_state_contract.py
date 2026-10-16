#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile the production transport.state formatter and validate its wire contract."""

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class TransportStateContractTests(unittest.TestCase):
    def test_production_snprintf_routes_match_the_wire_contract(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        production = (ROOT / "platform/ev_control_transport.c").read_text(encoding="utf-8")
        start = production.index("static esp_err_t ev_control_send_transport_state(uint32_t seq)")
        end = production.index("\nstatic esp_err_t ev_control_send_route_get", start)
        formatter = production[start:end]
        harness = r'''#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_SIZE -1
typedef enum { EV_STREAM_USER = 0, EV_STREAM_DEBUG = 1 } ev_stream_t;
typedef struct {
    bool has_owner;
    char session_id[33];
    uint32_t vm_epoch;
} ev_session_snapshot_t;

static esp_err_t ev_session_snapshot(ev_session_snapshot_t *snapshot)
{
    snapshot->has_owner = true;
    memcpy(snapshot->session_id, "0123456789abcdef0123456789abcdef", 33);
    snapshot->vm_epoch = 42;
    return ESP_OK;
}

static const char *ev_channel_get(ev_stream_t stream)
{
    (void)stream;
    return "tcp";
}

static const char *ev_control_bool_str(bool value)
{
    return value ? "true" : "false";
}

static bool ev_channel_ready(ev_stream_t stream)
{
    (void)stream;
    return true;
}

static const char *ev_channel_sink_state(const char *name)
{
    (void)name;
    return "ready";
}

static bool ev_channel_sink_supported(const char *name)
{
    (void)name;
    return true;
}

static bool ev_channel_sink_ready(const char *name)
{
    (void)name;
    return true;
}

static bool ev_channel_sink_present(const char *name)
{
    (void)name;
    return true;
}

static const char *ev_control_json_nullable_bool(bool supported, bool present)
{
    if (!supported) {
        return "null";
    }
    return present ? "true" : "false";
}

static esp_err_t ev_control_send_rpc_with_seq(const char *type, const char *method,
                                              const char *payload, uint32_t seq)
{
    (void)type;
    (void)method;
    (void)seq;
    puts(payload);
    return ESP_OK;
}

static const char ESP_VISION_TRANSPORT_DEFAULT_SINK[] = "usj";
static const char ESP_VISION_TRANSPORT_FALLBACK_SINK[] = "usb";

'''
        harness += formatter
        harness += r'''
int main(void)
{
    return ev_control_send_transport_state(1) == ESP_OK ? 0 : 1;
}
'''
        with tempfile.TemporaryDirectory(prefix="evmux-transport-state-") as directory:
            source = Path(directory) / "transport_state.c"
            binary = Path(directory) / "transport_state"
            source.write_text(harness, encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(source), "-o", str(binary)], check=True, capture_output=True,
                           text=True, cwd=ROOT)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True,
                                    cwd=ROOT)

        state = json.loads(result.stdout)
        self.assertEqual(state["owner"], True)
        self.assertEqual(state["sessionId"], "0123456789abcdef0123456789abcdef")
        self.assertEqual(state["vmEpoch"], 42)
        self.assertNotIn("routes", state)
        self.assertEqual(state["user"], {"route": "tcp", "auto": False, "ready": True})
        self.assertEqual(state["debug"], {"route": "tcp", "auto": False, "ready": True})


if __name__ == "__main__":
    unittest.main()

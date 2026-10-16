/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ev_control_json.h"

static bool parse_case(const char *kind, const char *json)
{
    if (strcmp(kind, "metadata") == 0) {
        ev_control_json_metadata_t metadata;
        return ev_control_json_parse_metadata(json, strlen(json), &metadata);
    }
    if (strcmp(kind, "hello") == 0) {
        ev_control_json_hello_t hello;
        return ev_control_json_parse_hello(json, strlen(json), &hello);
    }
    if (strcmp(kind, "rpcPayload") == 0) {
        uint8_t payload[2048];
        size_t encoded_length = strlen(json);
        if ((encoded_length % 2) != 0 || (encoded_length / 2) > sizeof(payload)) {
            return false;
        }
        size_t length = encoded_length / 2;
        for (size_t i = 0; i < length; ++i) {
            char byte[3] = {json[2 * i], json[2 * i + 1], '\0'};
            char *end = NULL;
            unsigned long value = strtoul(byte, &end, 16);
            if ((end != byte + 2) || (value > UINT8_MAX)) {
                return false;
            }
            payload[i] = (uint8_t)value;
        }
        return ev_control_json_rpc_payload_valid(payload, length);
    }
    return false;
}

static void test_metadata_top_level_and_duplicates(void)
{
    static const char nested_only[] =
        "{\"outer\":{\"channel\":\"user.rpc\",\"type\":\"req\",\"seq\":1,\"method\":\"hello\"}}";
    static const char shadowed[] =
        "{\"channel\":\"user.rpc\",\"type\":\"req\",\"seq\":7,\"method\":\"hello\","
        "\"extra\":{\"channel\":false,\"seq\":\"bad\",\"method\":[]}}";
    static const char duplicate_channel[] =
        "{\"channel\":\"user.rpc\",\"channel\":false,\"type\":\"req\",\"seq\":1,\"method\":\"hello\"}";
    static const char duplicate_escaped_channel[] =
        "{\"channel\":\"user.rpc\",\"ch\\u0061nnel\":\"debug.rpc\",\"type\":\"req\","
        "\"seq\":1,\"method\":\"hello\"}";
    static const char duplicate_seq[] =
        "{\"channel\":\"user.rpc\",\"type\":\"req\",\"seq\":1,\"seq\":\"bad\",\"method\":\"hello\"}";
    ev_control_json_metadata_t metadata;

    assert(!ev_control_json_parse_metadata(nested_only, sizeof(nested_only) - 1, &metadata));
    assert(ev_control_json_parse_metadata(shadowed, sizeof(shadowed) - 1, &metadata));
    assert(strcmp(metadata.channel, "user.rpc") == 0);
    assert(metadata.seq == 7 && strcmp(metadata.method, "hello") == 0);
    assert(!ev_control_json_parse_metadata(duplicate_channel, sizeof(duplicate_channel) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(duplicate_escaped_channel,
                                           sizeof(duplicate_escaped_channel) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(duplicate_seq, sizeof(duplicate_seq) - 1, &metadata));
}

static void test_metadata_types_and_complete_json(void)
{
    static const char duplicate_encoding[] =
        "{\"channel\":\"repl.stdin\",\"type\":\"req\",\"seq\":0,\"encoding\":\"binary\","
        "\"encoding\":false}";
    static const char wrong_channel[] = "{\"channel\":1,\"type\":\"req\",\"seq\":0}";
    static const char wrong_type[] = "{\"channel\":\"repl.stdin\",\"type\":true,\"seq\":0}";
    static const char wrong_seq[] = "{\"channel\":\"repl.stdin\",\"type\":\"req\",\"seq\":\"0\"}";
    static const char wrong_method[] =
        "{\"channel\":\"user.rpc\",\"type\":\"req\",\"seq\":0,\"method\":false}";
    static const char wrong_sid[] =
        "{\"channel\":\"repl.stdin\",\"type\":\"req\",\"seq\":0,\"sid\":1}";
    static const char wrong_encoding[] =
        "{\"channel\":\"repl.stdin\",\"type\":\"req\",\"seq\":0,\"encoding\":null}";
    static const char wrong_session_id[] =
        "{\"channel\":\"user.rpc\",\"type\":\"req\",\"seq\":0,\"method\":\"session.keepalive\","
        "\"sessionId\":null}";
    static const char wrong_epoch[] =
        "{\"channel\":\"repl.stdin\",\"type\":\"req\",\"seq\":0,\"vmEpoch\":1.0}";
    static const char missing_method[] =
        "{\"channel\":\"debug.rpc\",\"type\":\"req\",\"seq\":0}";
    static const char trailing[] =
        "{\"channel\":\"user.rpc\",\"type\":\"req\",\"seq\":0,\"method\":\"hello\"} trailing";
    static const char malformed[] =
        "{\"channel\":\"user.rpc\",\"type\":\"req\",\"seq\":0,\"method\":\"hello\",}";
    static const char vm_epoch_overflow[] =
        "{\"channel\":\"repl.stdin\",\"type\":\"req\",\"seq\":0,\"vmEpoch\":4294967296}";
    ev_control_json_metadata_t metadata;

    assert(ev_control_json_parse_metadata("{\"channel\":\"repl.stdin\",\"type\":\"req\",\"seq\":0}",
                                          strlen("{\"channel\":\"repl.stdin\",\"type\":\"req\",\"seq\":0}"),
                                          &metadata));
    assert(!ev_control_json_parse_metadata(wrong_channel, sizeof(wrong_channel) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(wrong_type, sizeof(wrong_type) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(wrong_seq, sizeof(wrong_seq) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(wrong_method, sizeof(wrong_method) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(duplicate_encoding, sizeof(duplicate_encoding) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(wrong_sid, sizeof(wrong_sid) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(wrong_encoding, sizeof(wrong_encoding) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(wrong_session_id, sizeof(wrong_session_id) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(wrong_epoch, sizeof(wrong_epoch) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(missing_method, sizeof(missing_method) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(trailing, sizeof(trailing) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(malformed, sizeof(malformed) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(vm_epoch_overflow, sizeof(vm_epoch_overflow) - 1, &metadata));
}

static void test_escaped_fields_and_uint32_boundaries(void)
{
    static const char escaped[] =
        "{\"ch\\u0061nnel\":\"user.rpc\",\"type\":\"req\",\"seq\":4294967295,"
        "\"method\":\"hell\\u006f\",\"session\\u0049d\":\"0123456789abcdef0123456789abcdef\","
        "\"vmEpoch\":4294967295}";
    static const char seq_overflow[] =
        "{\"channel\":\"user.rpc\",\"type\":\"req\",\"seq\":4294967296,\"method\":\"hello\"}";
    static const char seq_negative[] =
        "{\"channel\":\"user.rpc\",\"type\":\"req\",\"seq\":-1,\"method\":\"hello\"}";
    static const char seq_exponent[] =
        "{\"channel\":\"user.rpc\",\"type\":\"req\",\"seq\":1e0,\"method\":\"hello\"}";
    ev_control_json_metadata_t metadata;

    assert(ev_control_json_parse_metadata(escaped, sizeof(escaped) - 1, &metadata));
    assert(metadata.seq == UINT32_MAX && metadata.vm_epoch == UINT32_MAX);
    assert(strcmp(metadata.channel, "user.rpc") == 0 && strcmp(metadata.method, "hello") == 0);
    assert(metadata.has_session_id && strcmp(metadata.session_id, "0123456789abcdef0123456789abcdef") == 0);
    assert(!ev_control_json_parse_metadata(seq_overflow, sizeof(seq_overflow) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(seq_negative, sizeof(seq_negative) - 1, &metadata));
    assert(!ev_control_json_parse_metadata(seq_exponent, sizeof(seq_exponent) - 1, &metadata));
}

static void test_hello_payload_schema(void)
{
    static const char discovery[] = "{}";
    static const char discover_wrong_types[] =
        "{\"acquire\":false,\"evMuxVersion\":\"bad\",\"clientNonce\":7}";
    static const char duplicate_acquire[] =
        "{\"acquire\":false,\"\\u0061cquire\":true}";
    static const char acquire_valid[] =
        "{\"acquire\":true,\"evMuxVersion\":4,\"clientNonce\":\"0123456789abcdef0123456789abcdef\"}";
    static const char acquire_escaped_nonce[] =
        "{\"acquire\":true,\"evMuxVersion\":4,\"clientNonce\":\"\\u0030123456789abcdef0123456789abcdef\"}";
    static const char acquire_wrong_nonce[] =
        "{\"acquire\":true,\"evMuxVersion\":4,\"clientNonce\":7}";
    static const char version_overflow[] =
        "{\"acquire\":false,\"evMuxVersion\":4294967296}";
    static const char trailing[] = "{}garbage";
    static const char malformed[] = "{\"acquire\":false,}";
    ev_control_json_hello_t hello;

    assert(ev_control_json_parse_hello(discovery, sizeof(discovery) - 1, &hello));
    assert(!hello.has_acquire && !hello.has_ev_mux_version && !hello.has_client_nonce);
    assert(!ev_control_json_parse_hello(discover_wrong_types, sizeof(discover_wrong_types) - 1, &hello));
    assert(!ev_control_json_parse_hello(duplicate_acquire, sizeof(duplicate_acquire) - 1, &hello));
    assert(ev_control_json_parse_hello(acquire_valid, sizeof(acquire_valid) - 1, &hello));
    assert(hello.acquire && hello.ev_mux_version == 4 &&
           strcmp(hello.client_nonce, "0123456789abcdef0123456789abcdef") == 0);
    assert(ev_control_json_parse_hello(acquire_escaped_nonce, sizeof(acquire_escaped_nonce) - 1, &hello));
    assert(strcmp(hello.client_nonce, "0123456789abcdef0123456789abcdef") == 0);
    assert(!ev_control_json_parse_hello(acquire_wrong_nonce, sizeof(acquire_wrong_nonce) - 1, &hello));
    assert(!ev_control_json_parse_hello(version_overflow, sizeof(version_overflow) - 1, &hello));
    assert(!ev_control_json_parse_hello(trailing, sizeof(trailing) - 1, &hello));
    assert(!ev_control_json_parse_hello(malformed, sizeof(malformed) - 1, &hello));
}

static void test_rpc_payload_embedded_nul(void)
{
    static const uint8_t valid_json[] = {'{', '"', 'x', '"', ':', '1', '}'};
    static const uint8_t nul_before_trailing_json[] = {'{', '}', '\0', '{', '"', 'x', '"', ':', '1', '}'};

    assert(ev_control_json_rpc_payload_valid(valid_json, sizeof(valid_json)));
    assert(!ev_control_json_rpc_payload_valid(nul_before_trailing_json,
                                              sizeof(nul_before_trailing_json)));
}

int main(int argc, char **argv)
{
    if (argc == 4) {
        bool expected = strcmp(argv[2], "true") == 0;
        return parse_case(argv[1], argv[3]) == expected ? 0 : 1;
    }
    assert(argc == 1);
    test_metadata_top_level_and_duplicates();
    test_metadata_types_and_complete_json();
    test_escaped_fields_and_uint32_boundaries();
    test_hello_payload_schema();
    test_rpc_payload_embedded_nul();
    puts("EV-MUX JSON validation checks passed");
    return 0;
}

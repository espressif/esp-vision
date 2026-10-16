/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "network_service_json.h"

static void test_escaped_credentials(void)
{
    static const char json[] =
        "{\"action\":\"network.configure\",\"operationId\":\"0123456789abcdef0123456789abcdef\","
        "\"ssid\":\"lab\\\"\\\\ssid\",\"password\":\"pass\\\\word123\"}";
    network_json_request_t request;
    assert(parse_network_json(json, sizeof(json) - 1, &request));
    assert(request.has_operation_id && request.has_ssid && request.has_password);
    assert(strcmp(request.ssid, "lab\"\\ssid") == 0);
    assert(strcmp(request.password, "pass\\word123") == 0);
}

static void test_utf8_and_surrogate_pair(void)
{
    static const char json[] =
        "{\"action\":\"network.configure\",\"operationId\":\"0123456789abcdef0123456789abcdef\","
        "\"ssid\":\"caf\\u00e9-\\ud83d\\ude00\",\"password\":\"password\"}";
    static const uint8_t expected[] = "caf\xc3\xa9-\xf0\x9f\x98\x80";
    network_json_request_t request;
    assert(parse_network_json(json, sizeof(json) - 1, &request));
    assert(request.ssid_length == sizeof(expected) - 1);
    assert(memcmp(request.ssid, expected, sizeof(expected)) == 0);
}

static void test_string_boundaries(void)
{
    char json[512];
    static const char prefix[] = "{\"action\":\"network.configure\",\"operationId\":\"0123456789abcdef0123456789abcdef\",\"ssid\":\"";
    network_json_request_t request;
    memset(json, 0, sizeof(json));
    strcpy(json, prefix);
    size_t length = sizeof(prefix) - 1;
    memset(json + length, 's', 32);
    length += 32;
    strcpy(json + length, "\",\"password\":\"");
    length += strlen(json + length);
    size_t password_offset = length;
    memset(json + length, 'a', 64);
    length += 64;
    strcpy(json + length, "\"}");
    length += 2;
    assert(parse_network_json(json, length, &request));
    assert(request.ssid_length == 32);
    assert(request.password_length == 64);

    memmove(json + password_offset + 65, json + password_offset + 64, length - password_offset - 64);
    json[password_offset + 64] = 'a';
    assert(!parse_network_json(json, length + 1, &request));

    strcpy(json, prefix);
    length = sizeof(prefix) - 1;
    size_t ssid_offset = length;
    memset(json + length, 's', 32);
    length += 32;
    strcpy(json + length, "\",\"password\":\"\"}");
    length += strlen(json + length);
    memmove(json + ssid_offset + 33, json + ssid_offset + 32, length - ssid_offset - 32);
    json[ssid_offset + 32] = 's';
    assert(!parse_network_json(json, length + 1, &request));
}

static void test_duplicate_known_fields(void)
{
    static const char *const cases[] = {
        "{\"action\":\"network.status\",\"action\":\"network.status\"}",
        "{\"action\":\"network.configure\",\"operationId\":\"0123456789abcdef0123456789abcdef\","
        "\"operationId\":\"0123456789abcdef0123456789abcdef\",\"ssid\":\"x\",\"password\":\"\"}",
        "{\"action\":\"network.configure\",\"operationId\":\"0123456789abcdef0123456789abcdef\","
        "\"ssid\":\"x\",\"ssid\":\"y\",\"password\":\"\"}",
        "{\"action\":\"network.configure\",\"operationId\":\"0123456789abcdef0123456789abcdef\","
        "\"ssid\":\"x\",\"password\":\"\",\"password\":\"\"}",
        "{\"action\":\"network.enable\",\"operationId\":\"0123456789abcdef0123456789abcdef\","
        "\"enabled\":true,\"enabled\":false}",
    };
    network_json_request_t request;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(!parse_network_json(cases[i], strlen(cases[i]), &request));
    }
}

static void test_long_unknown_field_name(void)
{
    static const char json[] =
        "{\"action\":\"network.status\",\"1234567890123456789012345678901\":[true,{\"nested\":null}]}";
    network_json_request_t request;
    assert(parse_network_json(json, sizeof(json) - 1, &request));
}

static void test_malformed_strings_and_types(void)
{
    static const char *const cases[] = {
        "{\"action\":\"network.configure\",\"operationId\":\"0123456789abcdef0123456789abcdef\","
        "\"ssid\":\"bad\\u0000ssid\",\"password\":\"password\"}",
        "{\"action\":\"network.configure\",\"operationId\":\"0123456789abcdef0123456789abcdef\","
        "\"ssid\":\"\\ud800\",\"password\":\"password\"}",
        "{\"action\":\"network.configure\",\"operationId\":\"0123456789abcdef0123456789abcdef\","
        "\"ssid\":\"x\",\"password\":false}",
        "{\"action\":\"network.enable\",\"operationId\":\"0123456789abcdef0123456789abcdef\","
        "\"enabled\":\"true\"}",
    };
    network_json_request_t request;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(!parse_network_json(cases[i], strlen(cases[i]), &request));
    }
    const uint8_t invalid_utf8[] = {
        '{', '"', 'a', 'c', 't', 'i', 'o', 'n', '"', ':', '"', 'n', 'e', 't', 'w', 'o', 'r', 'k', '.',
        's', 't', 'a', 't', 'u', 's', '"', ',', '"', 'x', '"', ':', '"', 0xc0, 0xaf, '"', '}',
    };
    assert(!parse_network_json((const char *)invalid_utf8, sizeof(invalid_utf8), &request));
}

static void test_network_action_detection(void)
{
    static const char malformed[] = "{\"action\":\"network.configure\",";
    static const char unrelated[] = "{\"action\":\"device.control.status\"}";
    assert(contains_network_action(malformed, sizeof(malformed) - 1));
    assert(!contains_network_action(unrelated, sizeof(unrelated) - 1));
}

static void test_operation_idempotence_match(void)
{
    static const char operation_id[] = "0123456789abcdef0123456789abcdef";
    static const char other_id[] = "fedcba9876543210fedcba9876543210";
    static const uint8_t fingerprint[32] = {1, 2, 3};
    static const uint8_t changed_fingerprint[32] = {1, 2, 4};
    assert(network_json_operation_match(operation_id, 1, fingerprint,
                                        false, "", 0, fingerprint,
                                        false, "", 0, fingerprint) == NETWORK_JSON_OPERATION_NEW);
    assert(network_json_operation_match(operation_id, 1, fingerprint,
                                        true, operation_id, 1, fingerprint,
                                        false, "", 0, fingerprint) == NETWORK_JSON_OPERATION_DUPLICATE_ACTIVE);
    assert(network_json_operation_match(operation_id, 1, fingerprint,
                                        true, operation_id, 1, changed_fingerprint,
                                        false, "", 0, fingerprint) == NETWORK_JSON_OPERATION_ID_CONFLICT);
    assert(network_json_operation_match(other_id, 1, fingerprint,
                                        true, operation_id, 1, fingerprint,
                                        false, "", 0, fingerprint) == NETWORK_JSON_OPERATION_BUSY);
    assert(network_json_operation_match(operation_id, 1, fingerprint,
                                        false, "", 0, fingerprint,
                                        true, operation_id, 1, fingerprint) == NETWORK_JSON_OPERATION_DUPLICATE_LAST);
    assert(network_json_operation_match(operation_id, 1, fingerprint,
                                        false, "", 0, fingerprint,
                                        true, operation_id, 2, fingerprint) == NETWORK_JSON_OPERATION_ID_CONFLICT);
}

int main(void)
{
    test_escaped_credentials();
    test_utf8_and_surrogate_pair();
    test_string_boundaries();
    test_duplicate_known_fields();
    test_long_unknown_field_name();
    test_malformed_strings_and_types();
    test_network_action_detection();
    test_operation_idempotence_match();
    puts("network service JSON parser checks passed");
    return 0;
}

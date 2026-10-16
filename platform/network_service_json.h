/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NETWORK_SSID_MAX 32u
#define NETWORK_PASSWORD_MAX 64u
#define NETWORK_OPERATION_ID_LENGTH 32u

typedef struct {
    const char *text;
    size_t length;
    size_t position;
} json_parser_t;

typedef struct {
    char action[32];
    char operation_id[NETWORK_OPERATION_ID_LENGTH + 1];
    char ssid[NETWORK_SSID_MAX + 1];
    size_t ssid_length;
    char password[NETWORK_PASSWORD_MAX + 1];
    size_t password_length;
    bool enabled;
    bool has_operation_id;
    bool has_ssid;
    bool has_password;
    bool has_enabled;
} network_json_request_t;

typedef enum {
    NETWORK_JSON_OPERATION_NEW = 0,
    NETWORK_JSON_OPERATION_DUPLICATE_ACTIVE,
    NETWORK_JSON_OPERATION_DUPLICATE_LAST,
    NETWORK_JSON_OPERATION_ID_CONFLICT,
    NETWORK_JSON_OPERATION_BUSY,
} network_json_operation_match_t;

static network_json_operation_match_t network_json_operation_match(
    const char *id, uint8_t action, const uint8_t fingerprint[32],
    bool has_active, const char *active_id, uint8_t active_action, const uint8_t active_fingerprint[32],
    bool has_last, const char *last_id, uint8_t last_action, const uint8_t last_fingerprint[32])
{
    if (has_active) {
        if (strcmp(id, active_id) != 0) {
            return NETWORK_JSON_OPERATION_BUSY;
        }
        return action == active_action && memcmp(fingerprint, active_fingerprint, 32) == 0 ?
               NETWORK_JSON_OPERATION_DUPLICATE_ACTIVE : NETWORK_JSON_OPERATION_ID_CONFLICT;
    }
    if (has_last && strcmp(id, last_id) == 0) {
        return action == last_action && memcmp(fingerprint, last_fingerprint, 32) == 0 ?
               NETWORK_JSON_OPERATION_DUPLICATE_LAST : NETWORK_JSON_OPERATION_ID_CONFLICT;
    }
    return NETWORK_JSON_OPERATION_NEW;
}

static void parser_whitespace(json_parser_t *parser)
{
    while (parser->position < parser->length) {
        char c = parser->text[parser->position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        parser->position++;
    }
}

static bool parser_hex4(json_parser_t *parser, uint16_t *value)
{
    uint16_t result = 0;
    if (parser->length - parser->position < 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        char c = parser->text[parser->position++];
        uint8_t digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }
        result = (uint16_t)((result << 4) | digit);
    }
    *value = result;
    return true;
}

static void parser_byte(char *output, size_t capacity, size_t *length, uint8_t byte, bool *overflow)
{
    if (output != NULL && *length < capacity) {
        output[*length] = (char)byte;
    } else if (output != NULL) {
        *overflow = true;
    }
    (*length)++;
}

static bool parser_codepoint(char *output, size_t capacity, size_t *length, uint32_t value, bool *overflow)
{
    if (value == 0 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        return false;
    }
    if (value < 0x80) {
        parser_byte(output, capacity, length, value, overflow);
    } else if (value < 0x800) {
        parser_byte(output, capacity, length, 0xc0 | (value >> 6), overflow);
        parser_byte(output, capacity, length, 0x80 | (value & 0x3f), overflow);
    } else if (value < 0x10000) {
        parser_byte(output, capacity, length, 0xe0 | (value >> 12), overflow);
        parser_byte(output, capacity, length, 0x80 | ((value >> 6) & 0x3f), overflow);
        parser_byte(output, capacity, length, 0x80 | (value & 0x3f), overflow);
    } else {
        parser_byte(output, capacity, length, 0xf0 | (value >> 18), overflow);
        parser_byte(output, capacity, length, 0x80 | ((value >> 12) & 0x3f), overflow);
        parser_byte(output, capacity, length, 0x80 | ((value >> 6) & 0x3f), overflow);
        parser_byte(output, capacity, length, 0x80 | (value & 0x3f), overflow);
    }
    return true;
}

static bool parser_raw_utf8(json_parser_t *parser, char *output, size_t capacity, size_t *length, bool *overflow)
{
    uint8_t lead = parser->text[parser->position++];
    uint32_t codepoint;
    uint8_t count;
    if (lead >= 0xc2 && lead <= 0xdf) {
        codepoint = lead & 0x1f;
        count = 1;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        codepoint = lead & 0x0f;
        count = 2;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        codepoint = lead & 0x07;
        count = 3;
    } else {
        return false;
    }
    if (parser->length - parser->position < count) {
        return false;
    }
    uint8_t encoded[4] = {lead, 0, 0, 0};
    for (uint8_t i = 0; i < count; i++) {
        uint8_t byte = parser->text[parser->position++];
        if ((byte & 0xc0) != 0x80) {
            return false;
        }
        encoded[i + 1] = byte;
        codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if ((count == 1 && codepoint < 0x80) || (count == 2 && codepoint < 0x800) ||
            (count == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return false;
    }
    for (uint8_t i = 0; i <= count; i++) {
        parser_byte(output, capacity, length, encoded[i], overflow);
    }
    return true;
}

/* Reads and decodes one JSON string; a null output validates without storing. */
static bool parser_string(json_parser_t *parser, char *output, size_t capacity, size_t *length, bool *overflow)
{
    size_t decoded_length = 0;
    bool did_overflow = false;
    if (parser->position >= parser->length || parser->text[parser->position++] != '"') {
        return false;
    }
    while (parser->position < parser->length) {
        uint8_t c = parser->text[parser->position];
        if (c == '"') {
            parser->position++;
            /* Callers reserve one byte beyond capacity for the terminator. */
            if (output != NULL && decoded_length <= capacity) {
                output[decoded_length] = '\0';
            }
            if (length != NULL) {
                *length = decoded_length;
            }
            if (overflow != NULL) {
                *overflow = did_overflow;
            }
            return true;
        }
        if (c < 0x20) {
            return false;
        }
        if (c != '\\') {
            if (c < 0x80) {
                parser_byte(output, capacity, &decoded_length, c, &did_overflow);
                parser->position++;
            } else if (!parser_raw_utf8(parser, output, capacity, &decoded_length, &did_overflow)) {
                return false;
            }
            continue;
        }
        parser->position++;
        if (parser->position >= parser->length) {
            return false;
        }
        char escape = parser->text[parser->position++];
        switch (escape) {
        case '"': parser_byte(output, capacity, &decoded_length, '"', &did_overflow); break;
        case '\\': parser_byte(output, capacity, &decoded_length, '\\', &did_overflow); break;
        case '/': parser_byte(output, capacity, &decoded_length, '/', &did_overflow); break;
        case 'b': parser_byte(output, capacity, &decoded_length, '\b', &did_overflow); break;
        case 'f': parser_byte(output, capacity, &decoded_length, '\f', &did_overflow); break;
        case 'n': parser_byte(output, capacity, &decoded_length, '\n', &did_overflow); break;
        case 'r': parser_byte(output, capacity, &decoded_length, '\r', &did_overflow); break;
        case 't': parser_byte(output, capacity, &decoded_length, '\t', &did_overflow); break;
        case 'u': {
            uint16_t first;
            if (!parser_hex4(parser, &first)) {
                return false;
            }
            uint32_t codepoint = first;
            if (first >= 0xd800 && first <= 0xdbff) {
                uint16_t second;
                if (parser->length - parser->position < 6 || parser->text[parser->position] != '\\' ||
                        parser->text[parser->position + 1] != 'u') {
                    return false;
                }
                parser->position += 2;
                if (!parser_hex4(parser, &second) || second < 0xdc00 || second > 0xdfff) {
                    return false;
                }
                codepoint = 0x10000 + (((uint32_t)first - 0xd800) << 10) + ((uint32_t)second - 0xdc00);
            } else if (first >= 0xdc00 && first <= 0xdfff) {
                return false;
            }
            if (!parser_codepoint(output, capacity, &decoded_length, codepoint, &did_overflow)) {
                return false;
            }
            break;
        }
        default: return false;
        }
    }
    return false;
}

static bool parser_literal(json_parser_t *parser, const char *literal)
{
    size_t length = strlen(literal);
    if (parser->length - parser->position < length ||
            memcmp(parser->text + parser->position, literal, length) != 0) {
        return false;
    }
    parser->position += length;
    return true;
}

static bool parser_number(json_parser_t *parser)
{
    size_t start = parser->position;
    if (parser->position < parser->length && parser->text[parser->position] == '-') {
        parser->position++;
    }
    if (parser->position >= parser->length) {
        return false;
    }
    if (parser->text[parser->position] == '0') {
        parser->position++;
    } else if (parser->text[parser->position] >= '1' && parser->text[parser->position] <= '9') {
        do {
            parser->position++;
        } while (parser->position < parser->length &&
                 parser->text[parser->position] >= '0' && parser->text[parser->position] <= '9');
    } else {
        return false;
    }
    if (parser->position < parser->length && parser->text[parser->position] == '.') {
        parser->position++;
        size_t fractional = parser->position;
        while (parser->position < parser->length &&
                parser->text[parser->position] >= '0' && parser->text[parser->position] <= '9') {
            parser->position++;
        }
        if (fractional == parser->position) {
            return false;
        }
    }
    if (parser->position < parser->length &&
            (parser->text[parser->position] == 'e' || parser->text[parser->position] == 'E')) {
        parser->position++;
        if (parser->position < parser->length &&
                (parser->text[parser->position] == '+' || parser->text[parser->position] == '-')) {
            parser->position++;
        }
        size_t exponent = parser->position;
        while (parser->position < parser->length &&
                parser->text[parser->position] >= '0' && parser->text[parser->position] <= '9') {
            parser->position++;
        }
        if (exponent == parser->position) {
            return false;
        }
    }
    return parser->position > start;
}

static bool parser_skip_value(json_parser_t *parser, uint8_t depth);

static bool parser_skip_object(json_parser_t *parser, uint8_t depth)
{
    parser->position++;
    parser_whitespace(parser);
    if (parser->position < parser->length && parser->text[parser->position] == '}') {
        parser->position++;
        return true;
    }
    while (parser->position < parser->length) {
        size_t ignored_length;
        bool ignored_overflow;
        if (!parser_string(parser, NULL, 0, &ignored_length, &ignored_overflow)) {
            return false;
        }
        parser_whitespace(parser);
        if (parser->position >= parser->length || parser->text[parser->position++] != ':') {
            return false;
        }
        parser_whitespace(parser);
        if (!parser_skip_value(parser, depth + 1)) {
            return false;
        }
        parser_whitespace(parser);
        if (parser->position >= parser->length) {
            return false;
        }
        char separator = parser->text[parser->position++];
        if (separator == '}') {
            return true;
        }
        if (separator != ',') {
            return false;
        }
        parser_whitespace(parser);
    }
    return false;
}

static bool parser_skip_array(json_parser_t *parser, uint8_t depth)
{
    parser->position++;
    parser_whitespace(parser);
    if (parser->position < parser->length && parser->text[parser->position] == ']') {
        parser->position++;
        return true;
    }
    while (parser->position < parser->length) {
        if (!parser_skip_value(parser, depth + 1)) {
            return false;
        }
        parser_whitespace(parser);
        if (parser->position >= parser->length) {
            return false;
        }
        char separator = parser->text[parser->position++];
        if (separator == ']') {
            return true;
        }
        if (separator != ',') {
            return false;
        }
        parser_whitespace(parser);
    }
    return false;
}

static bool parser_skip_value(json_parser_t *parser, uint8_t depth)
{
    if (depth > 12 || parser->position >= parser->length) {
        return false;
    }
    char c = parser->text[parser->position];
    if (c == '"') {
        size_t ignored_length;
        bool ignored_overflow;
        return parser_string(parser, NULL, 0, &ignored_length, &ignored_overflow);
    }
    if (c == '{') {
        return parser_skip_object(parser, depth);
    }
    if (c == '[') {
        return parser_skip_array(parser, depth);
    }
    if (c == 't') {
        return parser_literal(parser, "true");
    }
    if (c == 'f') {
        return parser_literal(parser, "false");
    }
    if (c == 'n') {
        return parser_literal(parser, "null");
    }
    return parser_number(parser);
}

static bool parser_known_string(json_parser_t *parser, char *output, size_t capacity, size_t *length)
{
    bool overflow = false;
    if (!parser_string(parser, output, capacity - 1, length, &overflow)) {
        return false;
    }
    return !overflow;
}

static bool parse_network_json(const char *json, size_t json_length, network_json_request_t *request)
{
    if (json == NULL || request == NULL || json_length == 0 || json_length > 2048) {
        return false;
    }
    memset(request, 0, sizeof(*request));
    json_parser_t parser = {.text = json, .length = json_length};
    parser_whitespace(&parser);
    if (parser.position >= parser.length || parser.text[parser.position++] != '{') {
        return false;
    }
    bool has_action = false;
    bool seen_operation_id = false;
    bool seen_ssid = false;
    bool seen_password = false;
    bool seen_enabled = false;
    parser_whitespace(&parser);
    if (parser.position < parser.length && parser.text[parser.position] == '}') {
        return false;
    }
    while (parser.position < parser.length) {
        char key[32];
        size_t key_length;
        bool key_overflow;
        if (!parser_string(&parser, key, sizeof(key) - 1, &key_length, &key_overflow)) {
            return false;
        }
        parser_whitespace(&parser);
        if (parser.position >= parser.length || parser.text[parser.position++] != ':') {
            return false;
        }
        parser_whitespace(&parser);
        if (!key_overflow && strcmp(key, "action") == 0) {
            if (has_action || !parser_known_string(&parser, request->action, sizeof(request->action), NULL)) {
                return false;
            }
            has_action = true;
        } else if (!key_overflow && strcmp(key, "operationId") == 0) {
            if (seen_operation_id || !parser_known_string(&parser, request->operation_id,
                                                          sizeof(request->operation_id), NULL)) {
                return false;
            }
            seen_operation_id = true;
            request->has_operation_id = true;
        } else if (!key_overflow && strcmp(key, "ssid") == 0) {
            if (seen_ssid || !parser_known_string(&parser, request->ssid, sizeof(request->ssid),
                                                  &request->ssid_length)) {
                return false;
            }
            seen_ssid = true;
            request->has_ssid = true;
        } else if (!key_overflow && strcmp(key, "password") == 0) {
            if (seen_password || !parser_known_string(&parser, request->password,
                                                      sizeof(request->password), &request->password_length)) {
                return false;
            }
            seen_password = true;
            request->has_password = true;
        } else if (!key_overflow && strcmp(key, "enabled") == 0) {
            if (seen_enabled) {
                return false;
            }
            if (parser_literal(&parser, "true")) {
                request->enabled = true;
            } else if (parser_literal(&parser, "false")) {
                request->enabled = false;
            } else {
                return false;
            }
            seen_enabled = true;
            request->has_enabled = true;
        } else if (!parser_skip_value(&parser, 0)) {
            return false;
        }
        parser_whitespace(&parser);
        if (parser.position >= parser.length) {
            return false;
        }
        char separator = parser.text[parser.position++];
        if (separator == '}') {
            break;
        }
        if (separator != ',') {
            return false;
        }
        parser_whitespace(&parser);
    }
    parser_whitespace(&parser);
    return parser.position == parser.length && has_action && strncmp(request->action, "network.", 8) == 0;
}

static bool contains_network_action(const char *json, size_t length)
{
    if (json == NULL || length < 8) {
        return false;
    }
    for (size_t i = 0; i + 8 <= length; i++) {
        if (memcmp(json + i, "network.", 8) == 0) {
            return true;
        }
    }
    return false;
}

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

#define EV_CONTROL_JSON_METHOD_MAX (31)
#define EV_CONTROL_JSON_CHANNEL_CAPACITY (24)
#define EV_CONTROL_JSON_TYPE_CAPACITY (8)
#define EV_CONTROL_JSON_SESSION_ID_LENGTH (32)

typedef enum {
    EV_CONTROL_JSON_STRING = 0,
    EV_CONTROL_JSON_UINT32,
    EV_CONTROL_JSON_BOOLEAN,
} ev_control_json_type_t;

typedef struct {
    const char *name;
    ev_control_json_type_t type;
    bool required;
    bool present;
    union {
        struct {
            char *buffer;
            size_t capacity;
            size_t length;
        } string;
        uint32_t uint32;
        bool boolean;
    } value;
} ev_control_json_field_t;

typedef struct {
    char channel[EV_CONTROL_JSON_CHANNEL_CAPACITY];
    char type[EV_CONTROL_JSON_TYPE_CAPACITY];
    char method[EV_CONTROL_JSON_METHOD_MAX + 1];
    char session_id[EV_CONTROL_JSON_SESSION_ID_LENGTH + 1];
    uint32_t seq;
    uint32_t vm_epoch;
    bool has_method;
    bool has_sid;
    bool has_encoding;
    bool has_session_id;
    bool has_vm_epoch;
} ev_control_json_metadata_t;

typedef struct {
    bool acquire;
    bool has_acquire;
    uint32_t ev_mux_version;
    bool has_ev_mux_version;
    char client_nonce[EV_CONTROL_JSON_SESSION_ID_LENGTH + 1];
    bool has_client_nonce;
} ev_control_json_hello_t;

static inline bool ev_control_json_rpc_payload_valid(const uint8_t *payload, size_t length)
{
    return (payload != NULL) && (memchr(payload, '\0', length) == NULL);
}

typedef struct {
    const char *text;
    size_t length;
    size_t position;
} ev_control_json_parser_t;

static inline void ev_control_json_whitespace(ev_control_json_parser_t *parser)
{
    while (parser->position < parser->length) {
        char c = parser->text[parser->position];
        if ((c != ' ') && (c != '\t') && (c != '\r') && (c != '\n')) {
            break;
        }
        ++parser->position;
    }
}

static inline void ev_control_json_append(uint8_t byte, char *output, size_t capacity,
                                          size_t *length, bool *overflow)
{
    if (output != NULL) {
        if (*length + 1 < capacity) {
            output[*length] = (char)byte;
        } else {
            *overflow = true;
        }
    }
    ++(*length);
}

static inline bool ev_control_json_append_codepoint(uint32_t codepoint, char *output,
                                                    size_t capacity, size_t *length,
                                                    bool *overflow, bool *has_nul)
{
    if ((codepoint == 0) || (codepoint > 0x10ffff) ||
            ((codepoint >= 0xd800) && (codepoint <= 0xdfff))) {
        if (codepoint == 0) {
            *has_nul = true;
            return true;
        }
        return false;
    }
    if (codepoint < 0x80) {
        ev_control_json_append((uint8_t)codepoint, output, capacity, length, overflow);
    } else if (codepoint < 0x800) {
        ev_control_json_append((uint8_t)(0xc0 | (codepoint >> 6)), output, capacity, length, overflow);
        ev_control_json_append((uint8_t)(0x80 | (codepoint & 0x3f)), output, capacity, length, overflow);
    } else if (codepoint < 0x10000) {
        ev_control_json_append((uint8_t)(0xe0 | (codepoint >> 12)), output, capacity, length, overflow);
        ev_control_json_append((uint8_t)(0x80 | ((codepoint >> 6) & 0x3f)), output, capacity, length, overflow);
        ev_control_json_append((uint8_t)(0x80 | (codepoint & 0x3f)), output, capacity, length, overflow);
    } else {
        ev_control_json_append((uint8_t)(0xf0 | (codepoint >> 18)), output, capacity, length, overflow);
        ev_control_json_append((uint8_t)(0x80 | ((codepoint >> 12) & 0x3f)), output, capacity, length, overflow);
        ev_control_json_append((uint8_t)(0x80 | ((codepoint >> 6) & 0x3f)), output, capacity, length, overflow);
        ev_control_json_append((uint8_t)(0x80 | (codepoint & 0x3f)), output, capacity, length, overflow);
    }
    return true;
}

static inline bool ev_control_json_hex4(ev_control_json_parser_t *parser, uint16_t *out_value)
{
    uint16_t value = 0;
    if ((out_value == NULL) || (parser->length - parser->position < 4)) {
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        char c = parser->text[parser->position++];
        uint8_t digit;
        if ((c >= '0') && (c <= '9')) {
            digit = (uint8_t)(c - '0');
        } else if ((c >= 'a') && (c <= 'f')) {
            digit = (uint8_t)(c - 'a' + 10);
        } else if ((c >= 'A') && (c <= 'F')) {
            digit = (uint8_t)(c - 'A' + 10);
        } else {
            return false;
        }
        value = (uint16_t)((value << 4) | digit);
    }
    *out_value = value;
    return true;
}

static inline bool ev_control_json_append_raw_utf8(ev_control_json_parser_t *parser,
                                                   char *output, size_t capacity,
                                                   size_t *length, bool *overflow)
{
    uint8_t encoded[4];
    uint8_t lead = (uint8_t)parser->text[parser->position++];
    uint32_t codepoint;
    size_t trailing;
    if ((lead >= 0xc2) && (lead <= 0xdf)) {
        codepoint = lead & 0x1f;
        trailing = 1;
    } else if ((lead >= 0xe0) && (lead <= 0xef)) {
        codepoint = lead & 0x0f;
        trailing = 2;
    } else if ((lead >= 0xf0) && (lead <= 0xf4)) {
        codepoint = lead & 0x07;
        trailing = 3;
    } else {
        return false;
    }
    if (parser->length - parser->position < trailing) {
        return false;
    }
    encoded[0] = lead;
    for (size_t i = 0; i < trailing; ++i) {
        uint8_t byte = (uint8_t)parser->text[parser->position++];
        if ((byte & 0xc0) != 0x80) {
            return false;
        }
        encoded[i + 1] = byte;
        codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if (((trailing == 1) && (codepoint < 0x80)) ||
            ((trailing == 2) && (codepoint < 0x800)) ||
            ((trailing == 3) && (codepoint < 0x10000)) || (codepoint > 0x10ffff) ||
            ((codepoint >= 0xd800) && (codepoint <= 0xdfff))) {
        return false;
    }
    for (size_t i = 0; i <= trailing; ++i) {
        ev_control_json_append(encoded[i], output, capacity, length, overflow);
    }
    return true;
}

/* Decode one JSON string. Buffer capacity includes its terminating NUL. */
static inline bool ev_control_json_string(ev_control_json_parser_t *parser, char *output,
                                          size_t capacity, size_t *out_length,
                                          bool *out_overflow, bool *out_has_nul)
{
    size_t length = 0;
    bool overflow = false;
    bool has_nul = false;
    if ((parser->position >= parser->length) || (parser->text[parser->position++] != '"')) {
        return false;
    }
    while (parser->position < parser->length) {
        uint8_t c = (uint8_t)parser->text[parser->position];
        if (c == '"') {
            ++parser->position;
            if ((output != NULL) && (length < capacity)) {
                output[length] = '\0';
            }
            if (out_length != NULL) {
                *out_length = length;
            }
            if (out_overflow != NULL) {
                *out_overflow = overflow;
            }
            if (out_has_nul != NULL) {
                *out_has_nul = has_nul;
            }
            return true;
        }
        if (c < 0x20) {
            return false;
        }
        if (c != '\\') {
            if (c < 0x80) {
                ev_control_json_append(c, output, capacity, &length, &overflow);
                ++parser->position;
            } else if (!ev_control_json_append_raw_utf8(parser, output, capacity, &length, &overflow)) {
                return false;
            }
            continue;
        }
        ++parser->position;
        if (parser->position >= parser->length) {
            return false;
        }
        char escape = parser->text[parser->position++];
        uint8_t decoded;
        switch (escape) {
        case '"': decoded = '"'; break;
        case '\\': decoded = '\\'; break;
        case '/': decoded = '/'; break;
        case 'b': decoded = '\b'; break;
        case 'f': decoded = '\f'; break;
        case 'n': decoded = '\n'; break;
        case 'r': decoded = '\r'; break;
        case 't': decoded = '\t'; break;
        case 'u': {
            uint16_t first;
            if (!ev_control_json_hex4(parser, &first)) {
                return false;
            }
            uint32_t codepoint = first;
            if ((first >= 0xd800) && (first <= 0xdbff)) {
                uint16_t second;
                if ((parser->length - parser->position < 6) ||
                        (parser->text[parser->position] != '\\') ||
                        (parser->text[parser->position + 1] != 'u')) {
                    return false;
                }
                parser->position += 2;
                if (!ev_control_json_hex4(parser, &second) ||
                        (second < 0xdc00) || (second > 0xdfff)) {
                    return false;
                }
                codepoint = 0x10000 + (((uint32_t)first - 0xd800) << 10) +
                            ((uint32_t)second - 0xdc00);
            } else if ((first >= 0xdc00) && (first <= 0xdfff)) {
                return false;
            }
            if (!ev_control_json_append_codepoint(codepoint, output, capacity, &length,
                                                  &overflow, &has_nul)) {
                return false;
            }
            continue;
        }
        default: return false;
        }
        ev_control_json_append(decoded, output, capacity, &length, &overflow);
    }
    return false;
}

static inline bool ev_control_json_number(ev_control_json_parser_t *parser, uint32_t *out_value)
{
    uint32_t value = 0;
    if ((parser->position >= parser->length) ||
            (parser->text[parser->position] < '0') || (parser->text[parser->position] > '9')) {
        return false;
    }
    if (parser->text[parser->position] == '0') {
        ++parser->position;
        if ((parser->position < parser->length) &&
                (parser->text[parser->position] >= '0') && (parser->text[parser->position] <= '9')) {
            return false;
        }
    } else {
        while ((parser->position < parser->length) &&
                (parser->text[parser->position] >= '0') && (parser->text[parser->position] <= '9')) {
            uint32_t digit = (uint32_t)(parser->text[parser->position++] - '0');
            if (value > (UINT32_MAX - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
        }
    }
    if (out_value != NULL) {
        *out_value = value;
    }
    return true;
}

static inline bool ev_control_json_literal(ev_control_json_parser_t *parser, const char *literal)
{
    size_t length = strlen(literal);
    if ((parser->length - parser->position < length) ||
            (memcmp(parser->text + parser->position, literal, length) != 0)) {
        return false;
    }
    parser->position += length;
    return true;
}

static inline bool ev_control_json_skip_value(ev_control_json_parser_t *parser, uint8_t depth);

static inline bool ev_control_json_skip_object(ev_control_json_parser_t *parser, uint8_t depth)
{
    ++parser->position;
    ev_control_json_whitespace(parser);
    if ((parser->position < parser->length) && (parser->text[parser->position] == '}')) {
        ++parser->position;
        return true;
    }
    for (;;) {
        if (!ev_control_json_string(parser, NULL, 0, NULL, NULL, NULL)) {
            return false;
        }
        ev_control_json_whitespace(parser);
        if ((parser->position >= parser->length) || (parser->text[parser->position++] != ':')) {
            return false;
        }
        ev_control_json_whitespace(parser);
        if (!ev_control_json_skip_value(parser, (uint8_t)(depth + 1))) {
            return false;
        }
        ev_control_json_whitespace(parser);
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
        ev_control_json_whitespace(parser);
    }
}

static inline bool ev_control_json_skip_array(ev_control_json_parser_t *parser, uint8_t depth)
{
    ++parser->position;
    ev_control_json_whitespace(parser);
    if ((parser->position < parser->length) && (parser->text[parser->position] == ']')) {
        ++parser->position;
        return true;
    }
    for (;;) {
        if (!ev_control_json_skip_value(parser, (uint8_t)(depth + 1))) {
            return false;
        }
        ev_control_json_whitespace(parser);
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
        ev_control_json_whitespace(parser);
    }
}

static inline bool ev_control_json_skip_value(ev_control_json_parser_t *parser, uint8_t depth)
{
    if ((depth > 12) || (parser->position >= parser->length)) {
        return false;
    }
    char c = parser->text[parser->position];
    if (c == '"') {
        return ev_control_json_string(parser, NULL, 0, NULL, NULL, NULL);
    }
    if (c == '{') {
        return ev_control_json_skip_object(parser, depth);
    }
    if (c == '[') {
        return ev_control_json_skip_array(parser, depth);
    }
    if (c == 't') {
        return ev_control_json_literal(parser, "true");
    }
    if (c == 'f') {
        return ev_control_json_literal(parser, "false");
    }
    if (c == 'n') {
        return ev_control_json_literal(parser, "null");
    }
    /* Unknown extension values may use any valid JSON number. */
    size_t start = parser->position;
    if ((parser->position < parser->length) && (parser->text[parser->position] == '-')) {
        ++parser->position;
    }
    if (parser->position >= parser->length) {
        return false;
    }
    if (parser->text[parser->position] == '0') {
        ++parser->position;
        if ((parser->position < parser->length) &&
                (parser->text[parser->position] >= '0') && (parser->text[parser->position] <= '9')) {
            return false;
        }
    } else if ((parser->text[parser->position] >= '1') && (parser->text[parser->position] <= '9')) {
        do {
            ++parser->position;
        } while ((parser->position < parser->length) &&
                 (parser->text[parser->position] >= '0') && (parser->text[parser->position] <= '9'));
    } else {
        return false;
    }
    if ((parser->position < parser->length) && (parser->text[parser->position] == '.')) {
        ++parser->position;
        size_t fractional = parser->position;
        while ((parser->position < parser->length) &&
                (parser->text[parser->position] >= '0') && (parser->text[parser->position] <= '9')) {
            ++parser->position;
        }
        if (fractional == parser->position) {
            return false;
        }
    }
    if ((parser->position < parser->length) &&
            ((parser->text[parser->position] == 'e') || (parser->text[parser->position] == 'E'))) {
        ++parser->position;
        if ((parser->position < parser->length) &&
                ((parser->text[parser->position] == '+') || (parser->text[parser->position] == '-'))) {
            ++parser->position;
        }
        size_t exponent = parser->position;
        while ((parser->position < parser->length) &&
                (parser->text[parser->position] >= '0') && (parser->text[parser->position] <= '9')) {
            ++parser->position;
        }
        if (exponent == parser->position) {
            return false;
        }
    }
    return parser->position > start;
}

static inline ev_control_json_field_t ev_control_json_string_field(const char *name,
                                                                   bool required,
                                                                   char *buffer,
                                                                   size_t capacity)
{
    ev_control_json_field_t field = {
        .name = name,
        .type = EV_CONTROL_JSON_STRING,
        .required = required,
        .value.string = {.buffer = buffer, .capacity = capacity},
    };
    return field;
}

static inline ev_control_json_field_t ev_control_json_uint32_field(const char *name,
                                                                   bool required)
{
    ev_control_json_field_t field = {.name = name, .type = EV_CONTROL_JSON_UINT32, .required = required};
    return field;
}

static inline ev_control_json_field_t ev_control_json_bool_field(const char *name, bool required)
{
    ev_control_json_field_t field = {.name = name, .type = EV_CONTROL_JSON_BOOLEAN, .required = required};
    return field;
}

static inline bool ev_control_json_parse_object(const char *text, size_t length,
                                                ev_control_json_field_t *fields, size_t field_count)
{
    if ((text == NULL) || (length == 0) || ((field_count != 0) && (fields == NULL))) {
        return false;
    }
    for (size_t i = 0; i < field_count; ++i) {
        fields[i].present = false;
        if (fields[i].type == EV_CONTROL_JSON_STRING) {
            fields[i].value.string.length = 0;
            if ((fields[i].value.string.buffer != NULL) && (fields[i].value.string.capacity != 0)) {
                fields[i].value.string.buffer[0] = '\0';
            }
        } else if (fields[i].type == EV_CONTROL_JSON_UINT32) {
            fields[i].value.uint32 = 0;
        } else {
            fields[i].value.boolean = false;
        }
    }

    ev_control_json_parser_t parser = {.text = text, .length = length};
    ev_control_json_whitespace(&parser);
    if ((parser.position >= parser.length) || (parser.text[parser.position++] != '{')) {
        return false;
    }
    ev_control_json_whitespace(&parser);
    if ((parser.position < parser.length) && (parser.text[parser.position] == '}')) {
        ++parser.position;
    } else {
        for (;;) {
            char key[32];
            size_t key_length = 0;
            bool key_overflow = false;
            bool key_has_nul = false;
            if (!ev_control_json_string(&parser, key, sizeof(key), &key_length,
                                        &key_overflow, &key_has_nul)) {
                return false;
            }
            ev_control_json_whitespace(&parser);
            if ((parser.position >= parser.length) || (parser.text[parser.position++] != ':')) {
                return false;
            }
            ev_control_json_whitespace(&parser);
            ev_control_json_field_t *field = NULL;
            if (!key_overflow && !key_has_nul) {
                for (size_t i = 0; i < field_count; ++i) {
                    if ((key_length == strlen(fields[i].name)) && (strcmp(key, fields[i].name) == 0)) {
                        field = &fields[i];
                        break;
                    }
                }
            }
            if (field == NULL) {
                if (!ev_control_json_skip_value(&parser, 0)) {
                    return false;
                }
            } else {
                if (field->present) {
                    return false;
                }
                if (field->type == EV_CONTROL_JSON_STRING) {
                    bool overflow = false;
                    bool has_nul = false;
                    if (!ev_control_json_string(&parser, field->value.string.buffer,
                                                field->value.string.capacity,
                                                &field->value.string.length, &overflow, &has_nul) ||
                            overflow || has_nul) {
                        return false;
                    }
                } else if (field->type == EV_CONTROL_JSON_UINT32) {
                    if (!ev_control_json_number(&parser, &field->value.uint32)) {
                        return false;
                    }
                } else {
                    if (parser.position < parser.length && parser.text[parser.position] == 't') {
                        if (!ev_control_json_literal(&parser, "true")) {
                            return false;
                        }
                        field->value.boolean = true;
                    } else if (parser.position < parser.length && parser.text[parser.position] == 'f') {
                        if (!ev_control_json_literal(&parser, "false")) {
                            return false;
                        }
                    } else {
                        return false;
                    }
                }
                field->present = true;
            }
            ev_control_json_whitespace(&parser);
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
            ev_control_json_whitespace(&parser);
        }
    }
    ev_control_json_whitespace(&parser);
    if (parser.position != parser.length) {
        return false;
    }
    for (size_t i = 0; i < field_count; ++i) {
        if (fields[i].required && !fields[i].present) {
            return false;
        }
    }
    return true;
}

static inline bool ev_control_json_parse_metadata(const char *text, size_t length,
                                                  ev_control_json_metadata_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    ev_control_json_field_t fields[] = {
        ev_control_json_string_field("channel", true, out->channel, sizeof(out->channel)),
        ev_control_json_string_field("type", true, out->type, sizeof(out->type)),
        ev_control_json_uint32_field("seq", true),
        ev_control_json_string_field("method", false, out->method, sizeof(out->method)),
        ev_control_json_string_field("sid", false, NULL, 0),
        ev_control_json_string_field("encoding", false, NULL, 0),
        ev_control_json_string_field("sessionId", false, out->session_id, sizeof(out->session_id)),
        ev_control_json_uint32_field("vmEpoch", false),
    };
    if (!ev_control_json_parse_object(text, length, fields, sizeof(fields) / sizeof(fields[0]))) {
        return false;
    }
    bool is_rpc = (strcmp(out->channel, "user.rpc") == 0) ||
                  (strcmp(out->channel, "debug.rpc") == 0);
    if (is_rpc && (!fields[3].present || (fields[3].value.string.length == 0))) {
        return false;
    }
    out->seq = fields[2].value.uint32;
    out->has_method = fields[3].present;
    out->has_sid = fields[4].present;
    out->has_encoding = fields[5].present;
    out->has_session_id = fields[6].present;
    out->vm_epoch = fields[7].value.uint32;
    out->has_vm_epoch = fields[7].present;
    return true;
}

static inline bool ev_control_json_parse_hello(const char *text, size_t length,
                                               ev_control_json_hello_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    ev_control_json_field_t fields[] = {
        ev_control_json_bool_field("acquire", false),
        ev_control_json_uint32_field("evMuxVersion", false),
        ev_control_json_string_field("clientNonce", false, out->client_nonce, sizeof(out->client_nonce)),
    };
    if (!ev_control_json_parse_object(text, length, fields, sizeof(fields) / sizeof(fields[0]))) {
        return false;
    }
    out->has_acquire = fields[0].present;
    out->acquire = fields[0].value.boolean;
    out->has_ev_mux_version = fields[1].present;
    out->ev_mux_version = fields[1].value.uint32;
    out->has_client_nonce = fields[2].present;
    return true;
}

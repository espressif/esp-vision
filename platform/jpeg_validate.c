/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "jpeg_validate.h"

#define JPEG_COMPONENT_MAX (4)

typedef struct {
    uint8_t frame_marker;
    uint8_t component_count;
    uint8_t component_ids[JPEG_COMPONENT_MAX];
    uint8_t quantizer_ids[JPEG_COMPONENT_MAX];
    uint8_t quantizer_precision[JPEG_COMPONENT_MAX];
    uint8_t quantizers;
    uint8_t dc_tables;
    uint8_t ac_tables;
    uint8_t initial_components;
    uint16_t restart_interval;
    bool saw_scan;
} jpeg_validation_state_t;

static uint16_t jpeg_validation_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static bool jpeg_validation_quantizers(jpeg_validation_state_t *state, const uint8_t *data, size_t size)
{
    size_t position = 0;
    while (position < size) {
        uint8_t table = data[position++];
        unsigned int precision = table >> 4;
        unsigned int id = table & 15;
        if ((precision > 1) || (id >= JPEG_COMPONENT_MAX)) {
            return false;
        }
        size_t bytes = (precision == 0) ? 64 : 128;
        if (bytes > size - position) {
            return false;
        }
        for (size_t offset = 0; offset < bytes; offset += precision + 1) {
            unsigned int value = (precision == 0) ? data[position + offset] :
                                 jpeg_validation_be16(data + position + offset);
            if (value == 0) {
                return false;
            }
        }
        position += bytes;
        state->quantizers |= 1U << id;
        state->quantizer_precision[id] = precision;
    }
    return position != 0;
}

static bool jpeg_validation_huffman(jpeg_validation_state_t *state, const uint8_t *data, size_t size)
{
    size_t position = 0;
    while (position < size) {
        uint8_t table = data[position++];
        unsigned int kind = table >> 4;
        unsigned int id = table & 15;
        if ((kind > 1) || (id >= JPEG_COMPONENT_MAX) || (size - position < 16)) {
            return false;
        }
        unsigned int symbols = 0;
        int slots = 1;
        for (size_t depth = 0; depth < 16; ++depth) {
            unsigned int count = data[position++];
            symbols += count;
            slots = (slots * 2) - (int)count;
            if ((slots < 0) || (symbols > 256)) {
                return false;
            }
        }
        // An all-ones Huffman code is reserved. An entirely occupied tree is
        // invalid even when it is not oversubscribed (T.81 Annex C).
        if ((symbols == 0) || (slots == 0) || (symbols > size - position)) {
            return false;
        }
        position += symbols;
        if (kind == 0) {
            state->dc_tables |= 1U << id;
        } else {
            state->ac_tables |= 1U << id;
        }
    }
    return position != 0;
}

static bool jpeg_validation_frame(jpeg_validation_state_t *state, uint8_t marker,
                                  const uint8_t *data, size_t size, uint32_t width, uint32_t height)
{
    if ((state->frame_marker != 0) || (size < 6) || (data[0] != 8) ||
            (jpeg_validation_be16(data + 1) != height) || (jpeg_validation_be16(data + 3) != width)) {
        return false;
    }
    uint8_t count = data[5];
    if ((count == 0) || (count > JPEG_COMPONENT_MAX) || (size != 6U + (3U * count))) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        uint8_t id = data[6 + (3 * i)];
        uint8_t sampling = data[7 + (3 * i)];
        uint8_t quantizer = data[8 + (3 * i)];
        if (((sampling >> 4) == 0) || ((sampling >> 4) > 4) ||
                ((sampling & 15) == 0) || ((sampling & 15) > 4) || (quantizer >= JPEG_COMPONENT_MAX)) {
            return false;
        }
        for (size_t previous = 0; previous < i; ++previous) {
            if (state->component_ids[previous] == id) {
                return false;
            }
        }
        state->component_ids[i] = id;
        state->quantizer_ids[i] = quantizer;
    }
    state->component_count = count;
    state->frame_marker = marker;
    return true;
}

static bool jpeg_validation_scan(jpeg_validation_state_t *state, const uint8_t *data, size_t size)
{
    if ((state->frame_marker == 0) || (size < 4)) {
        return false;
    }
    uint8_t count = data[0];
    if ((count == 0) || (count > state->component_count) || (size != 4U + (2U * count))) {
        return false;
    }
    uint8_t first = data[1 + (2 * count)];
    uint8_t last = data[2 + (2 * count)];
    uint8_t approximation = data[3 + (2 * count)];
    unsigned int high = approximation >> 4;
    unsigned int low = approximation & 15;
    bool progressive = state->frame_marker == 0xc2;
    if (!progressive) {
        if ((first != 0) || (last != 63) || (approximation != 0)) {
            return false;
        }
    } else if ((first > last) || (last > 63) || ((first == 0) && (last != 0)) ||
               ((first != 0) && (count != 1)) || (high > 13) || (low > 13) ||
               ((high != 0) && (high != low + 1))) {
        return false;
    }

    uint8_t selected = 0;
    for (size_t i = 0; i < count; ++i) {
        uint8_t id = data[1 + (2 * i)];
        uint8_t tables = data[2 + (2 * i)];
        unsigned int dc = tables >> 4;
        unsigned int ac = tables & 15;
        size_t component = 0;
        while ((component < state->component_count) && (state->component_ids[component] != id)) {
            ++component;
        }
        if ((component == state->component_count) || (selected & (1U << component)) ||
                (dc >= JPEG_COMPONENT_MAX) || (ac >= JPEG_COMPONENT_MAX)) {
            return false;
        }
        unsigned int quantizer = state->quantizer_ids[component];
        if (!(state->quantizers & (1U << quantizer)) || (state->quantizer_precision[quantizer] != 0)) {
            return false;
        }
        if ((!progressive || ((first == 0) && (high == 0))) && !(state->dc_tables & (1U << dc))) {
            return false;
        }
        if ((!progressive || (first != 0)) && !(state->ac_tables & (1U << ac))) {
            return false;
        }
        selected |= 1U << component;
    }
    if (!progressive || ((first == 0) && (high == 0))) {
        state->initial_components |= selected;
    }
    state->saw_scan = true;
    return true;
}

bool esp_vision_jpeg_validate(const uint8_t *data, size_t size, uint32_t width, uint32_t height)
{
    if ((data == NULL) || (size < 4) || (width == 0) || (height == 0) ||
            (width > UINT16_MAX) || (height > UINT16_MAX) || (data[0] != 0xff) || (data[1] != 0xd8)) {
        return false;
    }
    jpeg_validation_state_t state = {0};
    size_t position = 2;
    bool in_scan = false;
    bool scan_has_data = false;
    uint8_t next_restart = 0;
    while (position < size) {
        if (in_scan && (data[position] != 0xff)) {
            scan_has_data = true;
            ++position;
            continue;
        }
        size_t marker_start = position;
        if (data[position++] != 0xff) {
            return false;
        }
        while ((position < size) && (data[position] == 0xff)) {
            ++position;
        }
        if (position == size) {
            return false;
        }
        uint8_t marker = data[position++];
        if (in_scan) {
            if (marker == 0) {
                // FF00 encodes one data byte. Extra FF fill is only allowed
                // before actual markers, never before a stuffed zero byte.
                if (position - marker_start != 2) {
                    return false;
                }
                scan_has_data = true;
                continue;
            }
            if ((marker >= 0xd0) && (marker <= 0xd7)) {
                if ((state.restart_interval == 0) || !scan_has_data || (marker != 0xd0 + next_restart)) {
                    return false;
                }
                next_restart = (next_restart + 1) & 7;
                scan_has_data = false;
                continue;
            }
            if (!scan_has_data) {
                return false;
            }
            in_scan = false;
        }
        if (marker == 0xd9) {
            uint8_t all_components = (1U << state.component_count) - 1;
            return state.saw_scan && (state.initial_components == all_components) && (position == size);
        }
        if ((marker == 0) || (marker == 0xd8) || (size - position < 2)) {
            return false;
        }
        size_t length = jpeg_validation_be16(data + position);
        if ((length < 2) || (length > size - position)) {
            return false;
        }
        const uint8_t *segment = data + position + 2;
        size_t segment_size = length - 2;
        position += length;
        if ((marker == 0xc0) || (marker == 0xc1) || (marker == 0xc2)) {
            if (!jpeg_validation_frame(&state, marker, segment, segment_size, width, height)) {
                return false;
            }
        } else if (marker == 0xdb) {
            if (!jpeg_validation_quantizers(&state, segment, segment_size)) {
                return false;
            }
        } else if (marker == 0xc4) {
            if (!jpeg_validation_huffman(&state, segment, segment_size)) {
                return false;
            }
        } else if (marker == 0xdd) {
            if (segment_size != 2) {
                return false;
            }
            state.restart_interval = jpeg_validation_be16(segment);
        } else if (marker == 0xda) {
            if (!jpeg_validation_scan(&state, segment, segment_size)) {
                return false;
            }
            in_scan = true;
            scan_has_data = false;
            next_restart = 0;
        } else if (!((marker >= 0xe0) && (marker <= 0xef)) && (marker != 0xfe)) {
            return false;
        }
    }
    return false;
}

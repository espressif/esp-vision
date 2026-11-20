/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "uvc_logo.h"

#include <stdbool.h>
#include <stdlib.h>

#define UVC_LOGO_MAX_BYTES (32 * 1024)
#define UVC_LOGO_GLYPH_WIDTH (17)
#define UVC_LOGO_GLYPH_HEIGHT (7)

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t size;
    unsigned bits;
    unsigned bit_count;
    bool overflow;
} uvc_logo_writer_t;

static void uvc_logo_put_byte(uvc_logo_writer_t *writer, uint8_t byte)
{
    if (writer->size == writer->capacity) {
        writer->overflow = true;
        return;
    }
    if (writer->data != NULL) {
        writer->data[writer->size] = byte;
    }
    ++writer->size;
}

static void uvc_logo_put_word(uvc_logo_writer_t *writer, uint16_t word)
{
    uvc_logo_put_byte(writer, word >> 8);
    uvc_logo_put_byte(writer, word);
}

static void uvc_logo_put_bits(uvc_logo_writer_t *writer, unsigned bits, unsigned count)
{
    writer->bits = (writer->bits << count) | bits;
    writer->bit_count += count;
    while (writer->bit_count >= 8) {
        writer->bit_count -= 8;
        uint8_t byte = writer->bits >> writer->bit_count;
        uvc_logo_put_byte(writer, byte);
        if (byte == 0xff) {
            uvc_logo_put_byte(writer, 0);
        }
    }
    writer->bits &= (1U << writer->bit_count) - 1;
}

static void uvc_logo_headers(uvc_logo_writer_t *writer, uint16_t width, uint16_t height)
{
    static const uint8_t jfif[] = {
        'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
    };
    uvc_logo_put_word(writer, 0xffd8); // SOI
    uvc_logo_put_word(writer, 0xffe0); // APP0
    uvc_logo_put_word(writer, sizeof(jfif) + 2);
    for (size_t i = 0; i < sizeof(jfif); ++i) {
        uvc_logo_put_byte(writer, jfif[i]);
    }
    uvc_logo_put_word(writer, 0xffdb); // DQT, one 8-bit table
    uvc_logo_put_word(writer, 67);
    uvc_logo_put_byte(writer, 0);
    for (int i = 0; i < 64; ++i) {
        uvc_logo_put_byte(writer, 8);
    }

    // Match color camera frames from the first sample. Neutral chroma keeps
    // the artwork gray while the JPEG advertises three components.
    uvc_logo_put_word(writer, 0xffc0); // SOF0, baseline YCbCr 4:2:2
    uvc_logo_put_word(writer, 17);
    uvc_logo_put_byte(writer, 8);
    uvc_logo_put_word(writer, height);
    uvc_logo_put_word(writer, width);
    uvc_logo_put_byte(writer, 3);
    for (uint8_t component = 1; component <= 3; ++component) {
        uvc_logo_put_byte(writer, component);
        uvc_logo_put_byte(writer, component == 1 ? 0x21 : 0x11);
        uvc_logo_put_byte(writer, 0);
    }

    uvc_logo_put_word(writer, 0xffc4); // DHT, DC categories 0, 7, 8 and AC EOB
    uvc_logo_put_word(writer, 40);
    uvc_logo_put_byte(writer, 0);
    for (int length = 1; length <= 16; ++length) {
        uvc_logo_put_byte(writer, length <= 3 ? 1 : 0);
    }
    uvc_logo_put_byte(writer, 0);
    uvc_logo_put_byte(writer, 7);
    uvc_logo_put_byte(writer, 8);
    uvc_logo_put_byte(writer, 0x10);
    for (int length = 1; length <= 16; ++length) {
        uvc_logo_put_byte(writer, length == 1 ? 1 : 0);
    }
    uvc_logo_put_byte(writer, 0);

    uvc_logo_put_word(writer, 0xffda); // SOS, full baseline scan
    uvc_logo_put_word(writer, 12);
    uvc_logo_put_byte(writer, 3);
    for (uint8_t component = 1; component <= 3; ++component) {
        uvc_logo_put_byte(writer, component);
        uvc_logo_put_byte(writer, 0);
    }
    uvc_logo_put_byte(writer, 0);
    uvc_logo_put_byte(writer, 63);
    uvc_logo_put_byte(writer, 0);
}

static uint8_t uvc_logo_luminance(uint32_t x, uint32_t y, uint32_t scale)
{
    // Monochrome block artwork, evaluated at the JPEG block grid.
    static const uint8_t letters[3][UVC_LOGO_GLYPH_HEIGHT] = {
        {31, 16, 16, 30, 16, 16, 31}, // E
        {15, 16, 16, 14,  1,  1, 30}, // S
        {30, 17, 17, 30, 16, 16, 16}, // P
    };
    x /= scale;
    y /= scale;
    if (x >= UVC_LOGO_GLYPH_WIDTH || y >= UVC_LOGO_GLYPH_HEIGHT || x % 6 == 5) {
        return 32;
    }
    return (letters[x / 6][y] & (16U >> (x % 6))) ? 224 : 32;
}

static void uvc_logo_put_block(uvc_logo_writer_t *writer, int diff)
{
    if (diff == 0) {
        uvc_logo_put_bits(writer, 0, 1);
    } else {
        // The initial predictor differs by 96, and a change between the two
        // luma levels differs by 192. Neutral chroma always uses DC zero.
        unsigned magnitude = diff < 0 ? (unsigned) - diff : (unsigned)diff;
        unsigned category = magnitude < 128 ? 7 : 8;
        uvc_logo_put_bits(writer, category == 7 ? 2 : 6, category == 7 ? 2 : 3);
        unsigned value = diff < 0 ? ((1U << category) - 1 - magnitude) : magnitude;
        uvc_logo_put_bits(writer, value, category);
    }
    uvc_logo_put_bits(writer, 0, 1); // AC end of block
}

static size_t uvc_logo_encode(uvc_logo_writer_t *writer, uint16_t width, uint16_t height)
{
    uint32_t cols = ((uint32_t)width + 7) / 8;
    uint32_t rows = ((uint32_t)height + 7) / 8;
    uint32_t scale_x = cols / (UVC_LOGO_GLYPH_WIDTH + 6);
    uint32_t scale_y = rows / (UVC_LOGO_GLYPH_HEIGHT + 6);
    uint32_t scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale == 0) {
        scale = 1;
    }
    uint32_t origin_x = cols > UVC_LOGO_GLYPH_WIDTH * scale ? (cols - UVC_LOGO_GLYPH_WIDTH * scale) / 2 : 0;
    uint32_t origin_y = rows > UVC_LOGO_GLYPH_HEIGHT * scale ? (rows - UVC_LOGO_GLYPH_HEIGHT * scale) / 2 : 0;

    uvc_logo_headers(writer, width, height);
    int previous = 0;
    for (uint32_t y = 0; y < rows && !writer->overflow; ++y) {
        for (uint32_t x = 0; x < cols && !writer->overflow; x += 2) {
            // A constant 8×8 block has only a DC coefficient. The DCT's factor
            // of eight cancels the quantizer, leaving the level-shifted sample.
            int left = (int)uvc_logo_luminance(x - origin_x, y - origin_y, scale) - 128;
            int right = (int)uvc_logo_luminance(x + 1 - origin_x, y - origin_y, scale) - 128;
            uvc_logo_put_block(writer, left - previous);
            uvc_logo_put_block(writer, right - left);
            previous = right;
            uvc_logo_put_block(writer, 0); // Cb = 128
            uvc_logo_put_block(writer, 0); // Cr = 128
        }
    }
    if (writer->bit_count != 0) {
        unsigned pad = 8 - writer->bit_count;
        uvc_logo_put_bits(writer, (1U << pad) - 1, pad);
    }
    uvc_logo_put_word(writer, 0xffd9); // EOI
    return writer->overflow ? 0 : writer->size;
}

uint8_t *esp_vision_uvc_logo_create(uint16_t width, uint16_t height, size_t *size_out)
{
    if (size_out == NULL) {
        return NULL;
    }
    *size_out = 0;
    if (width == 0 || height == 0) {
        return NULL;
    }
    uvc_logo_writer_t counter = {.capacity = UVC_LOGO_MAX_BYTES};
    size_t capacity = uvc_logo_encode(&counter, width, height);
    if (capacity == 0) {
        return NULL;
    }
    uint8_t *jpeg = malloc(capacity);
    if (jpeg == NULL) {
        return NULL;
    }
    uvc_logo_writer_t writer = {.data = jpeg, .capacity = capacity};
    if (uvc_logo_encode(&writer, width, height) != capacity) {
        free(jpeg);
        return NULL;
    }
    *size_out = capacity;
    return jpeg;
}

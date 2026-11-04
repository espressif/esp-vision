/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Validate a standalone 8-bit Huffman JPEG's bounded structure, table references
// and dimensions. This does not decode or prove completeness of entropy data.
// The EOI must end the supplied buffer; trailing padding/junk is not accepted.
bool esp_vision_jpeg_validate(const uint8_t *data, size_t size, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

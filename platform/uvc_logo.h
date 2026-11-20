/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

// Return a generated JPEG owned by the caller and released with free().
// Includes no raw framebuffer and never retains the returned allocation.
uint8_t *esp_vision_uvc_logo_create(uint16_t width, uint16_t height, size_t *size_out);

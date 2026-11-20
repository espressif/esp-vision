/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct image image_t;

esp_err_t esp_vision_jpeg_encode(const image_t *image, int quality,
                                 uint8_t **data, size_t *size);

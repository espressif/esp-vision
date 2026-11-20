/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>

#define MALLOC_CAP_SPIRAM (1)
#define MALLOC_CAP_8BIT (2)

void *heap_caps_malloc(size_t size, int capabilities);
void *heap_caps_realloc(void *ptr, size_t size, int capabilities);

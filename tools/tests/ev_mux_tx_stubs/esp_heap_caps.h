/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#define MALLOC_CAP_SPIRAM 0x01
#define MALLOC_CAP_8BIT 0x02
#define MALLOC_CAP_INTERNAL 0x04

void *heap_caps_malloc(size_t size, unsigned int caps);
void heap_caps_free(void *pointer);

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_HOST_TEST_ESP_HEAP_CAPS_H
#define ESP_VISION_HOST_TEST_ESP_HEAP_CAPS_H

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_INTERNAL (1U << 0)
#define MALLOC_CAP_SPIRAM   (1U << 1)
#define MALLOC_CAP_8BIT     (1U << 2)

void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_largest_free_block(uint32_t caps);

#endif /* ESP_VISION_HOST_TEST_ESP_HEAP_CAPS_H */

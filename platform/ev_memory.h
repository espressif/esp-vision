/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_VISION_MEMORY_AUTO,
    ESP_VISION_MEMORY_PREFER_INTERNAL,
    ESP_VISION_MEMORY_PREFER_SPIRAM,
} esp_vision_memory_preference_t;

/* required_caps are capability requirements such as MALLOC_CAP_8BIT or DMA.
 * The helper selects INTERNAL versus SPIRAM using ALWAYSINTERNAL, and only
 * falls back between those two classes while retaining every requirement. */
void *esp_vision_memory_malloc(size_t size, uint32_t required_caps);
void *esp_vision_memory_malloc_preferred(size_t size,
                                         uint32_t required_caps,
                                         esp_vision_memory_preference_t preference);
void *esp_vision_memory_calloc(size_t count, size_t size, uint32_t required_caps);
void *esp_vision_memory_aligned_alloc(size_t alignment, size_t size, uint32_t required_caps);
void *esp_vision_memory_aligned_calloc(size_t alignment,
                                       size_t count,
                                       size_t size,
                                       uint32_t required_caps);
void *esp_vision_memory_realloc(void *ptr, size_t size, uint32_t required_caps);

/* Exact-capability variants intentionally skip the automatic class policy. */
void *esp_vision_memory_malloc_caps(size_t size, uint32_t caps);
void *esp_vision_memory_aligned_alloc_caps(size_t alignment, size_t size, uint32_t caps);

#ifdef __cplusplus
}
#endif

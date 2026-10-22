/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ev_memory.h"

#include <stdbool.h>

#include "sdkconfig.h"

#ifndef CONFIG_SPIRAM
#define CONFIG_SPIRAM (0)
#endif

#ifndef CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
#define CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (0)
#endif

typedef enum {
    ESP_VISION_MEMORY_OP_MALLOC,
    ESP_VISION_MEMORY_OP_CALLOC,
    ESP_VISION_MEMORY_OP_ALIGNED_ALLOC,
    ESP_VISION_MEMORY_OP_ALIGNED_CALLOC,
} esp_vision_memory_op_t;

static size_t esp_vision_memory_get_caps_order(size_t size,
                                               uint32_t required_caps,
                                               esp_vision_memory_preference_t preference,
                                               uint32_t caps[2])
{
#if CONFIG_SPIRAM
    bool prefer_internal = preference == ESP_VISION_MEMORY_PREFER_INTERNAL;
    if (preference == ESP_VISION_MEMORY_AUTO) {
        prefer_internal = size <= CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL;
    }

    if (preference == ESP_VISION_MEMORY_PREFER_SPIRAM) {
        caps[0] = required_caps | MALLOC_CAP_SPIRAM;
        caps[1] = required_caps | MALLOC_CAP_INTERNAL;
    } else if (prefer_internal) {
        caps[0] = required_caps | MALLOC_CAP_INTERNAL;
        caps[1] = required_caps | MALLOC_CAP_SPIRAM;
    } else {
        caps[0] = required_caps | MALLOC_CAP_SPIRAM;
        caps[1] = required_caps | MALLOC_CAP_INTERNAL;
    }
    return 2;
#else
    (void)size;
    (void)preference;
    caps[0] = required_caps | MALLOC_CAP_INTERNAL;
    return 1;
#endif
}

static void *esp_vision_memory_allocate(esp_vision_memory_op_t operation,
                                        size_t alignment,
                                        size_t count,
                                        size_t size,
                                        uint32_t caps)
{
    switch (operation) {
    case ESP_VISION_MEMORY_OP_MALLOC:
        return heap_caps_malloc(size, caps);
    case ESP_VISION_MEMORY_OP_CALLOC:
        return heap_caps_calloc(count, size, caps);
    case ESP_VISION_MEMORY_OP_ALIGNED_ALLOC:
        return heap_caps_aligned_alloc(alignment, size, caps);
    case ESP_VISION_MEMORY_OP_ALIGNED_CALLOC:
        return heap_caps_aligned_calloc(alignment, count, size, caps);
    default:
        return NULL;
    }
}

static void *esp_vision_memory_allocate_preferred(esp_vision_memory_op_t operation,
                                                  size_t alignment,
                                                  size_t count,
                                                  size_t size,
                                                  uint32_t required_caps,
                                                  esp_vision_memory_preference_t preference)
{
    size_t request_size = size;
    if ((operation == ESP_VISION_MEMORY_OP_CALLOC) ||
            (operation == ESP_VISION_MEMORY_OP_ALIGNED_CALLOC)) {
        if ((size != 0) && (count > (SIZE_MAX / size))) {
            return NULL;
        }
        request_size = count * size;
    }

    uint32_t caps[2];
    size_t count_caps = esp_vision_memory_get_caps_order(request_size,
                                                         required_caps,
                                                         preference,
                                                         caps);
    for (size_t i = 0; i < count_caps; ++i) {
        void *ptr = esp_vision_memory_allocate(operation, alignment, count, size, caps[i]);
        if (ptr != NULL) {
            return ptr;
        }
    }
    return NULL;
}

void *esp_vision_memory_malloc(size_t size, uint32_t required_caps)
{
    return esp_vision_memory_malloc_preferred(size, required_caps, ESP_VISION_MEMORY_AUTO);
}

void *esp_vision_memory_malloc_preferred(size_t size,
                                         uint32_t required_caps,
                                         esp_vision_memory_preference_t preference)
{
    return esp_vision_memory_allocate_preferred(ESP_VISION_MEMORY_OP_MALLOC,
                                                0,
                                                0,
                                                size,
                                                required_caps,
                                                preference);
}

void *esp_vision_memory_calloc(size_t count, size_t size, uint32_t required_caps)
{
    return esp_vision_memory_allocate_preferred(ESP_VISION_MEMORY_OP_CALLOC,
                                                0,
                                                count,
                                                size,
                                                required_caps,
                                                ESP_VISION_MEMORY_AUTO);
}

void *esp_vision_memory_aligned_alloc(size_t alignment, size_t size, uint32_t required_caps)
{
    return esp_vision_memory_allocate_preferred(ESP_VISION_MEMORY_OP_ALIGNED_ALLOC,
                                                alignment,
                                                0,
                                                size,
                                                required_caps,
                                                ESP_VISION_MEMORY_AUTO);
}

void *esp_vision_memory_aligned_calloc(size_t alignment,
                                       size_t count,
                                       size_t size,
                                       uint32_t required_caps)
{
    return esp_vision_memory_allocate_preferred(ESP_VISION_MEMORY_OP_ALIGNED_CALLOC,
                                                alignment,
                                                count,
                                                size,
                                                required_caps,
                                                ESP_VISION_MEMORY_AUTO);
}

void *esp_vision_memory_realloc(void *ptr, size_t size, uint32_t required_caps)
{
    if (size == 0) {
        heap_caps_free(ptr);
        return NULL;
    }

    uint32_t caps[2];
    size_t count_caps = esp_vision_memory_get_caps_order(size,
                                                         required_caps,
                                                         ESP_VISION_MEMORY_AUTO,
                                                         caps);
    for (size_t i = 0; i < count_caps; ++i) {
        void *new_ptr = heap_caps_realloc(ptr, size, caps[i]);
        if (new_ptr != NULL) {
            return new_ptr;
        }
    }
    return NULL;
}

void *esp_vision_memory_malloc_caps(size_t size, uint32_t caps)
{
    return heap_caps_malloc(size, caps);
}

void *esp_vision_memory_aligned_alloc_caps(size_t alignment, size_t size, uint32_t caps)
{
    return heap_caps_aligned_alloc(alignment, size, caps);
}

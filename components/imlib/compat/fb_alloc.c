/*
 * SPDX-License-Identifier: MIT
 *
 * Heap-backed fb_alloc compatibility layer for the ESP32 OpenMV port.
 */

#include <string.h>

#include "esp_heap_caps.h"
#include "py/runtime.h"

#include "fb_alloc.h"
#include "omv_common.h"

typedef struct _omv_fb_alloc_node_t {
    struct _omv_fb_alloc_node_t *prev;
    void *raw_ptr;
    void *ptr;
    uint32_t size;
    uint8_t mark;
    uint8_t permanent;
} omv_fb_alloc_node_t;

static omv_fb_alloc_node_t *omv_fb_alloc_head = NULL;

static void *omv_fb_raw_alloc_with_caps(size_t size, uint32_t caps) {
    return heap_caps_malloc(size, caps);
}

static void *omv_fb_raw_alloc(size_t size, int hints) {
    void *ptr = NULL;

    if (hints & FB_ALLOC_PREFER_INTERNAL) {
        ptr = omv_fb_raw_alloc_with_caps(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ptr == NULL) {
            ptr = omv_fb_raw_alloc_with_caps(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    } else if (hints & FB_ALLOC_PREFER_SIZE) {
        ptr = omv_fb_raw_alloc_with_caps(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr == NULL) {
            ptr = omv_fb_raw_alloc_with_caps(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    } else {
        ptr = omv_fb_raw_alloc_with_caps(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr == NULL) {
            ptr = omv_fb_raw_alloc_with_caps(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }

    if (ptr == NULL) {
        ptr = omv_fb_raw_alloc_with_caps(size, MALLOC_CAP_8BIT);
    }

    return ptr;
}

static size_t omv_fb_largest_free_block_with_caps(uint32_t caps) {
    return heap_caps_get_largest_free_block(caps);
}

static size_t omv_fb_largest_free_block(int hints) {
    size_t preferred = 0;
    size_t fallback = 0;

    if (hints & FB_ALLOC_PREFER_INTERNAL) {
        preferred = omv_fb_largest_free_block_with_caps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        fallback = omv_fb_largest_free_block_with_caps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    } else if (hints & FB_ALLOC_PREFER_SIZE) {
        preferred = omv_fb_largest_free_block_with_caps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        fallback = omv_fb_largest_free_block_with_caps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    } else {
        preferred = omv_fb_largest_free_block_with_caps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        fallback = omv_fb_largest_free_block_with_caps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    size_t largest = preferred > fallback ? preferred : fallback;
    if (largest == 0) {
        largest = omv_fb_largest_free_block_with_caps(MALLOC_CAP_8BIT);
    }

    return largest;
}

static NORETURN void omv_fb_alloc_oom(void) {
    mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Out of fast frame buffer stack memory"));
}

char *fb_alloc_sp() {
    return NULL;
}

void fb_alloc_fail() {
    omv_fb_alloc_oom();
}

void fb_alloc_init0() {
    fb_free_all();
}

uint32_t fb_avail() {
    size_t largest = omv_fb_largest_free_block(FB_ALLOC_PREFER_SIZE);
    if (largest <= OMV_ALLOC_ALIGNMENT) {
        return 0;
    }
    return largest - OMV_ALLOC_ALIGNMENT;
}

void fb_alloc_mark() {
    omv_fb_alloc_node_t *node = m_new_obj(omv_fb_alloc_node_t);
    node->prev = omv_fb_alloc_head;
    node->raw_ptr = NULL;
    node->ptr = NULL;
    node->size = 0;
    node->mark = 1;
    node->permanent = 0;
    omv_fb_alloc_head = node;
}

static void omv_fb_alloc_pop_until_mark(bool free_permanent) {
    while (omv_fb_alloc_head != NULL) {
        omv_fb_alloc_node_t *node = omv_fb_alloc_head;
        omv_fb_alloc_head = node->prev;

        if (node->raw_ptr != NULL) {
            heap_caps_free(node->raw_ptr);
        }

        bool stop = node->mark && (!node->permanent || free_permanent);
        if (node->mark && node->permanent && !free_permanent) {
            omv_fb_alloc_head = node;
            return;
        }

        m_free(node);

        if (stop) {
            return;
        }
    }
}

void fb_alloc_free_till_mark() {
    omv_fb_alloc_pop_until_mark(false);
}

void fb_alloc_mark_permanent() {
    if (omv_fb_alloc_head != NULL) {
        omv_fb_alloc_head->permanent = 1;
    }
}

void fb_alloc_free_till_mark_past_mark_permanent() {
    omv_fb_alloc_pop_until_mark(true);
}

void *fb_alloc(uint32_t size, int hints) {
    if (size == 0) {
        return NULL;
    }

    omv_fb_alloc_node_t *node = m_new_obj(omv_fb_alloc_node_t);
    uint32_t alloc_size = OMV_ALIGN_TO(size, OMV_ALLOC_ALIGNMENT);
    alloc_size += OMV_ALLOC_ALIGNMENT - 1;
    node->prev = omv_fb_alloc_head;

    node->raw_ptr = omv_fb_raw_alloc(alloc_size, hints);
    node->ptr = node->raw_ptr;
    node->size = size;
    node->mark = 0;
    node->permanent = 0;

    if (node->raw_ptr == NULL) {
        m_free(node);
        omv_fb_alloc_oom();
    }

    uintptr_t aligned = OMV_ALIGN_TO((uintptr_t) node->raw_ptr, OMV_ALLOC_ALIGNMENT);
    node->ptr = (void *) aligned;

    omv_fb_alloc_head = node;
    return node->ptr;
}

void *fb_alloc0(uint32_t size, int hints) {
    void *mem = fb_alloc(size, hints);
    if (mem != NULL) {
        memset(mem, 0, size);
    }
    return mem;
}

void *fb_alloc_all(uint32_t *size, int hints) {
    size_t largest = omv_fb_largest_free_block(hints);
    size_t reserve = 32 * 1024;

    if (largest <= (reserve + OMV_ALLOC_ALIGNMENT)) {
        *size = 0;
        return NULL;
    }

    size_t request = largest - reserve;
    request = OMV_ALIGN_TO(request, OMV_ALLOC_ALIGNMENT);

    while (request >= OMV_ALLOC_ALIGNMENT) {
        void *mem = fb_alloc(request, hints);
        if (mem != NULL) {
            *size = request;
            return mem;
        }
        request -= OMV_ALLOC_ALIGNMENT;
    }

    *size = 0;
    return NULL;
}

void *fb_alloc0_all(uint32_t *size, int hints) {
    void *mem = fb_alloc_all(size, hints);
    if ((mem != NULL) && (*size != 0)) {
        memset(mem, 0, *size);
    }
    return mem;
}

void fb_free() {
    if (omv_fb_alloc_head == NULL) {
        return;
    }

    omv_fb_alloc_node_t *node = omv_fb_alloc_head;
    omv_fb_alloc_head = node->prev;
    if (node->raw_ptr != NULL) {
        heap_caps_free(node->raw_ptr);
    }
    m_free(node);
}

void fb_free_all() {
    while (omv_fb_alloc_head != NULL) {
        fb_free();
    }
}

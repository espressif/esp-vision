/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2007-2017 Ralph Hempel
 * Copyright (C) 2017-2024 OpenMV, LLC.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * UMM memory allocator.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "py/runtime.h"
#include "umm_malloc.h"

typedef union _umm_alloc_header_t umm_alloc_header_t;

union _umm_alloc_header_t {
    struct {
        umm_alloc_header_t *prev;
        umm_alloc_header_t *next;
        size_t size;
        bool mark;
    } node;
    max_align_t alignment;
};

static umm_alloc_header_t *s_umm_alloc_head;

void umm_alloc_fail(void) {
    mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("umm_malloc failed"));
}

void umm_init_x(size_t size) {
    (void) size;
}

void umm_deinit_x(void) {
    // Heap-backed UMM allocations are released individually, unlike the
    // original framebuffer-backed allocator's single arena allocation.
}

static void *umm_heap_malloc(size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *umm_heap_realloc(void *ptr, size_t size) {
    void *new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_ptr == NULL) {
        new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (new_ptr == NULL) {
        new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    }
    return new_ptr;
}

static void umm_alloc_link(umm_alloc_header_t *header) {
    header->node.prev = NULL;
    header->node.next = s_umm_alloc_head;
    if (s_umm_alloc_head != NULL) {
        s_umm_alloc_head->node.prev = header;
    }
    s_umm_alloc_head = header;
}

static void umm_alloc_unlink(umm_alloc_header_t *header) {
    if (header->node.prev != NULL) {
        header->node.prev->node.next = header->node.next;
    } else {
        s_umm_alloc_head = header->node.next;
    }
    if (header->node.next != NULL) {
        header->node.next->node.prev = header->node.prev;
    }
}

static umm_alloc_header_t *umm_alloc_new_node(size_t size, bool mark) {
    if (size > (SIZE_MAX - sizeof(umm_alloc_header_t))) {
        return NULL;
    }

    umm_alloc_header_t *header = umm_heap_malloc(sizeof(umm_alloc_header_t) + size);
    if (header == NULL) {
        return NULL;
    }

    header->node.size = size;
    header->node.mark = mark;
    umm_alloc_link(header);
    return header;
}

void umm_alloc_mark(void) {
    if (umm_alloc_new_node(0, true) == NULL) {
        umm_alloc_fail();
    }
}

void umm_alloc_free_till_mark(void) {
    umm_alloc_header_t *mark = s_umm_alloc_head;
    while ((mark != NULL) && !mark->node.mark) {
        mark = mark->node.next;
    }
    if (mark == NULL) {
        return;
    }

    while (s_umm_alloc_head != NULL) {
        umm_alloc_header_t *header = s_umm_alloc_head;
        bool stop = header->node.mark;
        umm_alloc_unlink(header);
        heap_caps_free(header);
        if (stop) {
            return;
        }
    }
}

void *umm_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    umm_alloc_header_t *header = umm_alloc_new_node(size, false);
    return header == NULL ? NULL : (void *)(header + 1);
}

void *umm_calloc(size_t num, size_t size) {
    if ((num == 0) || (size == 0)) {
        return NULL;
    }
    if (num > (SIZE_MAX / size)) {
        return NULL;
    }

    size_t total_size = num * size;
    void *ptr = umm_malloc(total_size);
    if (ptr != NULL) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

void *umm_realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return umm_malloc(size);
    }
    if (size == 0) {
        umm_free(ptr);
        return NULL;
    }
    if (size > (SIZE_MAX - sizeof(umm_alloc_header_t))) {
        return NULL;
    }

    umm_alloc_header_t *header = ((umm_alloc_header_t *)ptr) - 1;
    umm_alloc_header_t *new_header = umm_heap_realloc(header, sizeof(umm_alloc_header_t) + size);
    if (new_header == NULL) {
        return NULL;
    }

    if (new_header != header) {
        if (new_header->node.prev != NULL) {
            new_header->node.prev->node.next = new_header;
        } else {
            s_umm_alloc_head = new_header;
        }
        if (new_header->node.next != NULL) {
            new_header->node.next->node.prev = new_header;
        }
    }
    new_header->node.size = size;
    return (void *)(new_header + 1);
}

void umm_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    umm_alloc_header_t *header = ((umm_alloc_header_t *)ptr) - 1;
    umm_alloc_unlink(header);
    heap_caps_free(header);
}

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "fb_alloc.h"
#include "py/runtime.h"
#include "umm_malloc.h"

const mp_obj_type_t mp_type_MemoryError = { 0 };

static size_t s_internal_largest;
static size_t s_spiram_largest;
static size_t s_generic_largest;
static size_t s_heap_allocations;
static size_t s_m_allocations;
static uint32_t s_last_caps;

static size_t largest_for_caps(uint32_t caps)
{
    if (caps & MALLOC_CAP_INTERNAL) {
        return s_internal_largest;
    }
    if (caps & MALLOC_CAP_SPIRAM) {
        return s_spiram_largest;
    }
    return s_generic_largest;
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    if (size > largest_for_caps(caps)) {
        return NULL;
    }

    void *ptr = malloc(size);
    if (ptr != NULL) {
        s_heap_allocations++;
        s_last_caps = caps;
    }
    return ptr;
}

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    if (size > largest_for_caps(caps)) {
        return NULL;
    }

    void *new_ptr = realloc(ptr, size);
    if (new_ptr != NULL) {
        s_last_caps = caps;
    }
    return new_ptr;
}

void heap_caps_free(void *ptr)
{
    if (ptr != NULL) {
        assert(s_heap_allocations > 0);
        s_heap_allocations--;
        free(ptr);
    }
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return largest_for_caps(caps);
}

void *test_m_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (ptr != NULL) {
        s_m_allocations++;
    }
    return ptr;
}

void test_m_free(void *ptr)
{
    if (ptr != NULL) {
        assert(s_m_allocations > 0);
        s_m_allocations--;
        free(ptr);
    }
}

NORETURN void mp_raise_msg(const mp_obj_type_t *type, const char *message)
{
    (void)type;
    fprintf(stderr, "unexpected MicroPython exception: %s\n", message);
    abort();
}

static void reset_heap(size_t internal_largest, size_t spiram_largest)
{
    assert(s_heap_allocations == 0);
    assert(s_m_allocations == 0);
    s_internal_largest = internal_largest;
    s_spiram_largest = spiram_largest;
    s_generic_largest = internal_largest > spiram_largest ? internal_largest : spiram_largest;
    s_last_caps = 0;
}

static void test_fb_alloc_all_uses_usable_fallback(void)
{
    reset_heap(16U * 1024U, 1024U * 1024U);

    uint32_t size = 0;
    void *ptr = fb_alloc_all(&size, FB_ALLOC_PREFER_INTERNAL);
    assert(ptr != NULL);
    assert(size > (32U * 1024U));
    assert(s_last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    fb_free_all();
    assert(s_heap_allocations == 0);
    assert(s_m_allocations == 0);
}

static void test_heap_umm_lifecycle_does_not_pop_fb_mark(void)
{
    reset_heap(256U * 1024U, 1024U * 1024U);

    fb_alloc_mark();
    assert(fb_alloc(64, FB_ALLOC_NO_HINT) != NULL);
    fb_free();
    assert(s_m_allocations == 1);

    umm_init_x(1024);
    umm_deinit_x();
    assert(s_m_allocations == 1);

    fb_alloc_free_till_mark();
    assert(s_heap_allocations == 0);
    assert(s_m_allocations == 0);
}

static void test_combined_scope_restores_allocator_depth(void)
{
    reset_heap(256U * 1024U, 1024U * 1024U);

    fb_alloc_mark();
    umm_alloc_mark();
    assert(fb_alloc(96, FB_ALLOC_NO_HINT) != NULL);
    assert(umm_malloc(128) != NULL);

    umm_alloc_free_till_mark();
    fb_alloc_free_till_mark();
    assert(s_heap_allocations == 0);
    assert(s_m_allocations == 0);

    umm_alloc_free_till_mark();
    fb_alloc_free_till_mark();
    assert(s_heap_allocations == 0);
    assert(s_m_allocations == 0);
}

static void test_umm_scope_preserves_older_allocations(void)
{
    reset_heap(256U * 1024U, 1024U * 1024U);

    void *outside = umm_malloc(64);
    assert(outside != NULL);
    assert(s_heap_allocations == 1);

    umm_alloc_mark();
    void *first = umm_malloc(128);
    void *second = umm_calloc(4, 64);
    assert(first != NULL);
    assert(second != NULL);
    first = umm_realloc(first, 512);
    assert(first != NULL);
    umm_free(second);

    umm_alloc_free_till_mark();
    assert(s_heap_allocations == 1);

    umm_alloc_free_till_mark();
    assert(s_heap_allocations == 1);

    umm_free(outside);
    assert(s_heap_allocations == 0);
}

static void test_nested_umm_scope_restores_one_mark_at_a_time(void)
{
    reset_heap(256U * 1024U, 1024U * 1024U);

    umm_alloc_mark();
    assert(umm_malloc(32) != NULL);
    umm_alloc_mark();
    assert(umm_malloc(48) != NULL);

    umm_alloc_free_till_mark();
    assert(s_heap_allocations == 2);
    umm_alloc_free_till_mark();
    assert(s_heap_allocations == 0);
}

int main(void)
{
    test_fb_alloc_all_uses_usable_fallback();
    test_heap_umm_lifecycle_does_not_pop_fb_mark();
    test_combined_scope_restores_allocator_depth();
    test_umm_scope_preserves_older_allocations();
    test_nested_umm_scope_restores_one_mark_at_a_time();
    puts("imlib allocator host tests passed");
    return 0;
}

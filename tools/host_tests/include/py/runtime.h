/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_HOST_TEST_PY_RUNTIME_H
#define ESP_VISION_HOST_TEST_PY_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define NORETURN __attribute__((noreturn))
#define MP_ERROR_TEXT(text) (text)
#define m_new_obj(type) ((type *)test_m_malloc(sizeof(type)))
#define m_free(ptr) test_m_free(ptr)

typedef struct {
    int unused;
} mp_obj_type_t;

extern const mp_obj_type_t mp_type_MemoryError;

void *test_m_malloc(size_t size);
void test_m_free(void *ptr);
NORETURN void mp_raise_msg(const mp_obj_type_t *type, const char *message);

#endif /* ESP_VISION_HOST_TEST_PY_RUNTIME_H */

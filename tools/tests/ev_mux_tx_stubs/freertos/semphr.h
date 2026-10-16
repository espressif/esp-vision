/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    storage->taken = false;
    return storage;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    storage->taken = true;
    return storage;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    (void)timeout;
    StaticSemaphore_t *state = (StaticSemaphore_t *)semaphore;
    if (state->taken) {
        return pdFALSE;
    }
    state->taken = true;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    StaticSemaphore_t *state = (StaticSemaphore_t *)semaphore;
    state->taken = false;
    return pdTRUE;
}

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "freertos/FreeRTOS.h"

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    pthread_mutex_init(storage, NULL);
    return storage;
}

static inline int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned int timeout)
{
    (void)timeout;
    return pthread_mutex_lock(semaphore) == 0;
}

static inline int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return pthread_mutex_unlock(semaphore) == 0;
}

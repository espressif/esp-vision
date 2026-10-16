/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <pthread.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"

typedef struct {
    pthread_mutex_t mutex;
} StaticSemaphore_t;

typedef StaticSemaphore_t *SemaphoreHandle_t;

void ev_transport_test_semaphore_contended(void);

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    if (pthread_mutex_init(&storage->mutex, NULL) != 0) {
        return NULL;
    }
    return storage;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout)
{
    (void)timeout;
    int result = pthread_mutex_trylock(&semaphore->mutex);
    if (result != 0) {
        ev_transport_test_semaphore_contended();
        result = pthread_mutex_lock(&semaphore->mutex);
    }
    return result == 0 ? pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return pthread_mutex_unlock(&semaphore->mutex) == 0 ? pdTRUE : pdFALSE;
}

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_VISION_TEST_FREERTOS_SEMPHR_H
#define ESP_VISION_TEST_FREERTOS_SEMPHR_H

#include <stdbool.h>

#include "FreeRTOS.h"

typedef struct {
    bool taken;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    buffer->taken = false;
    return buffer;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks)
{
    (void)wait_ticks;
    if (semaphore == NULL || semaphore->taken) {
        return pdFALSE;
    }
    semaphore->taken = true;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL || !semaphore->taken) {
        return pdFALSE;
    }
    semaphore->taken = false;
    return pdTRUE;
}

#endif

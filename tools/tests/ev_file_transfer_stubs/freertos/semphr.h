/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "FreeRTOS.h"
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    pthread_mutex_init(buffer, NULL);
    return buffer;
}
static inline int xSemaphoreTake(SemaphoreHandle_t lock, unsigned int ticks)
{
    return (ticks ? pthread_mutex_lock(lock) : pthread_mutex_trylock(lock)) == 0;
}
static inline void xSemaphoreGive(SemaphoreHandle_t lock)
{
    pthread_mutex_unlock(lock);
}

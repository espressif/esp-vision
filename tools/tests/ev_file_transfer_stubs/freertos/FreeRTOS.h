/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <pthread.h>
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU

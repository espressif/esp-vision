/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

extern bool ev_mux_test_task_create_fail;

void ev_mux_test_delay_hook(TickType_t ticks);

static inline TaskHandle_t xTaskCreateStatic(TaskFunction_t task,
                                             const char *name,
                                             uint32_t stack_depth,
                                             void *argument,
                                             unsigned int priority,
                                             StackType_t *stack,
                                             StaticTask_t *task_buffer)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)argument;
    (void)priority;
    (void)stack;
    if (ev_mux_test_task_create_fail) {
        return NULL;
    }
    return task_buffer;
}

static inline void vTaskDelay(TickType_t ticks)
{
    ev_mux_test_delay_hook(ticks);
}

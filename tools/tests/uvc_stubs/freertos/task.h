/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "freertos/FreeRTOS.h"

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_size,
                       void *argument, unsigned priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);

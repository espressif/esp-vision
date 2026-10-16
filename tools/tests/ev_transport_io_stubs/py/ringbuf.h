/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

typedef struct {
    int unused;
} ringbuf_t;

int ringbuf_get(ringbuf_t *ringbuf);

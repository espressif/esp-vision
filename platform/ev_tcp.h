/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "ev_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the persistent, nonblocking IPv4 listener worker once. */
esp_err_t ev_tcp_init0(void);

/* FreeRTOS high-water mark for the worker, in bytes; zero if not running. */
uint32_t ev_tcp_task_stack_min_free_bytes(void);

/* Statically dispatched transport backend operations. */
ev_io_result_t ev_tcp_link_read(const ev_link_ref_t *link, void *buffer, size_t capacity);
ev_io_result_t ev_tcp_link_write(const ev_link_ref_t *link, const void *buffer, size_t length);
void ev_tcp_link_close(const ev_link_ref_t *link);

#ifdef __cplusplus
}
#endif

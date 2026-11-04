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
ev_io_result_t ev_tcp_link_writev(const ev_link_ref_t *link,
                                  const ev_io_vector_t *vectors, size_t count);
void ev_tcp_link_close(const ev_link_ref_t *link);

typedef struct {
    bool detached;
    bool linger_configured;
    bool abort_configured;
    bool close_attempted;
    int setup_errno;
    int fallback_errno;
    int close_errno;
} ev_tcp_reset_close_result_t;

/* Reset-only best effort: detach this exact link, then close its fd without
 * holding the socket lock. close success is NOT proof of peer/application ACK:
 * lwIP also returns success after a linger timeout abort. Call immediately
 * before restart, outside the TX worker and TCP/IP task. Other closes retain
 * their default disabled linger. A failed close is not retried. */
esp_err_t ev_tcp_link_close_for_reset(const ev_link_ref_t *link,
                                      ev_tcp_reset_close_result_t *result);

#ifdef __cplusplus
}
#endif

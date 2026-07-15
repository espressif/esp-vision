/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_STREAM_USER = 0,
    EV_STREAM_DEBUG,
    EV_STREAM_MAX,
} ev_stream_t;

typedef void (*ev_channel_route_changed_cb_t)(ev_stream_t stream,
                                              const char *sink_id,
                                              bool auto_route,
                                              const char *reason);

void ev_channel_init0(void);
void ev_channel_poll_auto(void);
void ev_channel_set_route_changed_cb(ev_channel_route_changed_cb_t callback);

esp_err_t ev_channel_bind(ev_stream_t stream, const char *sink_id);
esp_err_t ev_channel_bind_auto(ev_stream_t stream);
const char *ev_channel_get(ev_stream_t stream);
const char *ev_channel_sink_state(const char *sink_id);
bool ev_channel_is_auto(ev_stream_t stream);
bool ev_channel_ready(ev_stream_t stream);
bool ev_channel_sink_ready(const char *sink_id);
bool ev_channel_sink_present(const char *sink_id);
ssize_t ev_channel_write(ev_stream_t stream, const void *buf, size_t len);
ssize_t ev_channel_write_sink_if_present(const char *sink_id, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

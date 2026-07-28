/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ev_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ev_mux_init0(void);
esp_err_t ev_mux_write(ev_stream_t stream,
                       const char *metadata,
                       const void *payload,
                       size_t payload_len);
// Lossy variant: when the routed sink is congested (a previous write stalled),
// the whole frame is dropped before a single byte is written and
// ev_mux_tx_drop_count is bumped, so the wire stream stays parseable. Only
// for frames that may be dropped silently (preview); RPC/REPL must use
// ev_mux_write, which never drops.
esp_err_t ev_mux_write_lossy(ev_stream_t stream,
                             const char *metadata,
                             const void *payload,
                             size_t payload_len);
esp_err_t ev_mux_write_sink_if_present(const char *sink_id,
                                       const char *metadata,
                                       const void *payload,
                                       size_t payload_len);
// True while the sink currently routing `stream` is in the congestion
// cooldown. Producers can check this before investing in a payload (e.g.
// JPEG encode) that would be dropped at write time.
bool ev_mux_stream_congested(ev_stream_t stream);
void ev_mux_record_tx_drop(void);
uint32_t ev_mux_tx_timeout_count(void);
uint32_t ev_mux_tx_drop_count(void);

uint32_t ev_mux_crc32(const void *data, size_t len);
int ev_mux_snprintf_u32(char *buf, size_t len, uint32_t value);

#ifdef __cplusplus
}
#endif

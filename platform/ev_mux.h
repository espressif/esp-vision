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
#include "ev_transport.h"
#include "ev_vm_output.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ev_mux_init0(void);

#define EV_MUX_TX_CONTEXT_MAX (40)
#define EV_MUX_LARGE_PAYLOAD_MAX (256 * 1024)

typedef uint32_t ev_mux_ticket_t;

typedef enum {
    EV_MUX_TX_SESSION = 0,
    EV_MUX_TX_CONTROL,
    EV_MUX_TX_REPL,
    EV_MUX_TX_LOG,
    EV_MUX_TX_PREVIEW,
} ev_mux_tx_class_t;

/* The completion context is copied into bounded queue storage before this
 * call returns. The link argument is passed by value and is valid throughout
 * the callback only. Normal completions run on the TX worker; cancellation
 * completions may run on the cancelling task. Callbacks must be thread-safe,
 * must not block, and must not access MicroPython VM objects.
 */
typedef void (*ev_mux_completion_cb_t)(esp_err_t result,
                                       ev_link_ref_t link,
                                       const void *context,
                                       size_t context_len);

typedef struct {
    const ev_link_ref_t *link;
    const char *session_id;   // NULL for discovery frames not tied to a session.
    uint32_t vm_epoch;        // 0 for frames independent of a VM epoch.
    ev_mux_tx_class_t tx_class;
    ev_mux_completion_cb_t completion;
    const void *completion_context;
    size_t completion_context_len;
    // Optional execution origin for REPL and preview stream output. The
    // complete value is copied before the submit call returns.
    const ev_vm_output_origin_t *vm_output_origin;
} ev_mux_submit_options_t;

// Enqueues a complete frame and copies all caller-owned bytes before return.
// ticket_out is optional; when supplied, wait/forget may be used as a bounded
// completion barrier without retaining a pointer to caller storage.
esp_err_t ev_mux_submit_link(const ev_mux_submit_options_t *options,
                             const char *metadata,
                             const void *payload,
                             size_t payload_len,
                             ev_mux_ticket_t *ticket_out);

// Dedicated bounded pools for large debug responses and latest-only preview.
// Both functions copy payload bytes before returning. Each pool is allocated
// once on first use and reused; allocation failure does not affect ordinary TX.
esp_err_t ev_mux_submit_large_link(const ev_mux_submit_options_t *options,
                                   const char *metadata,
                                   const void *payload,
                                   size_t payload_len,
                                   ev_mux_ticket_t *ticket_out);
esp_err_t ev_mux_submit_preview_link(const ev_mux_submit_options_t *options,
                                     const char *metadata,
                                     const void *payload,
                                     size_t payload_len);
bool ev_mux_large_payload_available(void);
bool ev_mux_preview_slot_available(void);

esp_err_t ev_mux_wait(ev_mux_ticket_t ticket, uint32_t timeout_ms, esp_err_t *tx_result_out);
void ev_mux_ticket_forget(ev_mux_ticket_t ticket);

// Cancel queued, not-yet-started frames for a VM epoch or session. A frame
// already in progress is allowed to finish or hit its bounded deadline.
void ev_mux_cancel_vm_epoch(uint32_t vm_epoch);
void ev_mux_cancel_session(const ev_link_ref_t *link, const char *session_id);

esp_err_t ev_mux_write(ev_stream_t stream,
                       const char *metadata,
                       const void *payload,
                       size_t payload_len);
esp_err_t ev_mux_write_class(ev_stream_t stream,
                             ev_mux_tx_class_t tx_class,
                             const char *metadata,
                             const void *payload,
                             size_t payload_len);
// Lossy variant: when the routed sink is congested (a previous write stalled),
// the whole frame is dropped before a single byte is written and
// ev_mux_tx_drop_count is bumped. This avoids adding another partial frame
// while the host resynchronizes after the stalled write. Only for frames that
// may be dropped silently (preview); RPC/REPL must use ev_mux_write.
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
bool ev_mux_preview_available(ev_stream_t stream);
void ev_mux_record_tx_drop(void);
uint32_t ev_mux_tx_timeout_count(void);
uint32_t ev_mux_tx_drop_count(void);
uint32_t ev_mux_tx_queue_full_count(void);
uint32_t ev_mux_tx_stale_count(void);

uint32_t ev_mux_crc32(const void *data, size_t len);
int ev_mux_snprintf_u32(char *buf, size_t len, uint32_t value);

#ifdef __cplusplus
}
#endif

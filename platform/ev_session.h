/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ev_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EV_SESSION_ID_HEX_LEN (32)
#define EV_SESSION_NONCE_HEX_LEN (32)
#define EV_SESSION_LEASE_MS (15000U)
#define EV_SESSION_KEEPALIVE_MS (5000U)

typedef enum {
    EV_SESSION_FREE = 0,
    EV_SESSION_OWNED,
    EV_SESSION_RELEASING,
} ev_session_state_t;

typedef enum {
    EV_SESSION_RESULT_OK = 0,
    EV_SESSION_RESULT_BUSY,
    EV_SESSION_RESULT_SESSION_REQUIRED,
    EV_SESSION_RESULT_NOT_OWNER,
    EV_SESSION_RESULT_VM_BUSY,
    EV_SESSION_RESULT_VM_EPOCH_MISMATCH,
    EV_SESSION_RESULT_STALE_LINK,
    EV_SESSION_RESULT_INVALID_ARGUMENT,
} ev_session_result_t;

typedef enum {
    EV_SESSION_CHANGE_ACQUIRED = 0,
    EV_SESSION_CHANGE_RELEASED,
    EV_SESSION_CHANGE_EXPIRED,
    EV_SESSION_CHANGE_LINK_LOST,
    EV_SESSION_CHANGE_TX_FAILED,
} ev_session_change_reason_t;

typedef struct {
    ev_session_state_t state;
    bool has_owner;
    ev_link_ref_t owner_link;
    char session_id[EV_SESSION_ID_HEX_LEN + 1];
    char client_nonce[EV_SESSION_NONCE_HEX_LEN + 1];
    uint32_t lease_remaining_ms;
    uint32_t vm_epoch;
    // Changes whenever the owner or VM epoch changes. Fast consumers use this
    // token to reject stale queued input without taking the session mutex.
    uint32_t input_generation;
    uint32_t transition_id;
    bool handshake_complete;
    bool vm_ready;
    ev_transport_id_t user_route;
    ev_transport_id_t debug_route;
    uint8_t user_route_slot;
    uint8_t debug_route_slot;
} ev_session_snapshot_t;

typedef void (*ev_session_changed_cb_t)(const ev_session_snapshot_t *snapshot,
                                        ev_session_change_reason_t reason,
                                        uint32_t transition_id,
                                        const ev_link_ref_t *previous_link,
                                        const char *previous_session_id);

esp_err_t ev_session_init0(void);
void ev_session_set_changed_cb(ev_session_changed_cb_t callback);
bool ev_session_snapshot(ev_session_snapshot_t *out_snapshot);

// now_ms is monotonic milliseconds and is explicitly passed for deterministic
// state-machine tests. Production callers use esp_timer_get_time-derived time.
ev_session_result_t ev_session_acquire(const ev_link_ref_t *link,
                                       const char *client_nonce,
                                       uint32_t now_ms,
                                       ev_session_snapshot_t *out_snapshot);
ev_session_result_t ev_session_keepalive(const ev_link_ref_t *link,
                                         const char *session_id,
                                         uint32_t now_ms,
                                         ev_session_snapshot_t *out_snapshot);
ev_session_result_t ev_session_authorize(const ev_link_ref_t *link,
                                         const char *session_id,
                                         bool vm_related,
                                         bool has_vm_epoch,
                                         uint32_t vm_epoch,
                                         uint32_t now_ms,
                                         ev_session_snapshot_t *out_snapshot);

// Release is split around the accepted response: prepare blocks competing
// acquisition, then finish frees the owner even if the response write failed.
ev_session_result_t ev_session_release_prepare(const ev_link_ref_t *link,
                                               const char *session_id,
                                               ev_session_snapshot_t *out_snapshot);
bool ev_session_hello_complete(const ev_link_ref_t *link,
                               const char *session_id,
                               bool tx_succeeded);
bool ev_session_output_ready(const ev_link_ref_t *link, const char *session_id);
bool ev_session_release_finish(const ev_link_ref_t *link,
                               const char *session_id,
                               ev_session_change_reason_t reason);
bool ev_session_link_lost(const ev_link_ref_t *link,
                          ev_session_change_reason_t reason);
bool ev_session_tick(uint32_t now_ms);

uint32_t ev_session_vm_epoch(void);
uint32_t ev_session_vm_epoch_advance(void);
uint32_t ev_session_input_generation(void);
void ev_session_set_vm_ready(bool ready);
bool ev_session_vm_ready(void);

const char *ev_session_result_code(ev_session_result_t result);

#ifdef __cplusplus
}
#endif

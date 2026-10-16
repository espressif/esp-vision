/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ev_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EV_VM_OUTPUT_SCOPE_DEPTH_MAX (4U)

// A value-only identity for output produced by one accepted session input.
// It deliberately survives owner loss so TX can reject stale output instead
// of resolving it to a later session.
typedef struct {
    ev_link_ref_t link;
    char session_id[EV_SESSION_ID_HEX_LEN + 1];
    uint32_t vm_epoch;
    uint32_t input_generation;
    uint32_t transition_id;
} ev_vm_output_origin_t;

typedef struct {
    uint32_t cookie;
    uint32_t reset_generation;
    uint8_t depth;
    bool active;
} ev_vm_output_scope_t;

void ev_vm_output_init0(void);
void ev_vm_output_reset(void);
bool ev_vm_output_origin_from_session(const ev_session_snapshot_t *session,
                                      ev_vm_output_origin_t *out_origin);

// Scopes are task-local and copy all origin fields. The matching pop token
// restores the previous nested scope even when Python code raised an error.
bool ev_vm_output_scope_push(const ev_vm_output_origin_t *origin,
                             ev_vm_output_scope_t *scope);
void ev_vm_output_scope_pop(ev_vm_output_scope_t *scope);
bool ev_vm_output_scope_can_capture_thread(void);

// Returns true when output is bound to an owner-origin scope or an in-progress
// owned REPL line. The returned identity is the captured value, even if stale;
// this function never looks up or substitutes the current owner.
bool ev_vm_output_scope_snapshot(ev_vm_output_origin_t *out_origin);

// REPL/input source capture. A changed origin is rejected without replacing
// the active execution or edit origin, allowing the caller to return Ctrl-C
// without consuming a new owner's byte.
void ev_vm_output_repl_read_begin(void);
bool ev_vm_output_repl_input_accept(const ev_vm_output_origin_t *origin);
bool ev_vm_output_repl_read_complete(ev_vm_output_origin_t *out_origin);
void ev_vm_output_repl_read_abort(void);
bool ev_vm_output_repl_edit_stale(void);
bool ev_vm_output_input_origin_stale(void);

// Compare every captured session token with the current owner snapshot. This
// is used immediately before starting a REPL command; once execution starts,
// its scope remains bound to the captured identity until it exits.
bool ev_vm_output_origin_is_current(const ev_vm_output_origin_t *origin);

#ifdef __cplusplus
}
#endif

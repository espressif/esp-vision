/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ev_vm_output.h"

#include <string.h>

typedef struct {
    uint32_t cookie;
    ev_vm_output_origin_t origin;
} ev_vm_output_scope_entry_t;

typedef struct {
    uint32_t next_cookie;
    uint32_t reset_generation;
    uint8_t scope_depth;
    bool repl_editing;
    bool repl_has_origin;
    ev_vm_output_origin_t repl_origin;
    ev_vm_output_scope_entry_t scopes[EV_VM_OUTPUT_SCOPE_DEPTH_MAX];
} ev_vm_output_task_state_t;

// All scope and REPL edit state belongs to the MicroPython VM task. Producers
// running in other tasks have no inherited scope and keep default routing.
static __thread ev_vm_output_task_state_t s_task_state;

static bool ev_vm_output_session_id_valid(const char *session_id)
{
    if (session_id == NULL) {
        return false;
    }
    for (size_t i = 0; i < EV_SESSION_ID_HEX_LEN; ++i) {
        char c = session_id[i];
        if (!(((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')))) {
            return false;
        }
    }
    return session_id[EV_SESSION_ID_HEX_LEN] == '\0';
}

static bool ev_vm_output_origin_valid(const ev_vm_output_origin_t *origin)
{
    return (origin != NULL) &&
           ((unsigned int)origin->link.transport_id < EV_TRANSPORT_MAX) &&
           (origin->link.generation != 0) &&
           (origin->vm_epoch != 0) &&
           (origin->input_generation != 0) &&
           ev_vm_output_session_id_valid(origin->session_id);
}

static bool ev_vm_output_origin_equal(const ev_vm_output_origin_t *left,
                                      const ev_vm_output_origin_t *right)
{
    return (left != NULL) && (right != NULL) &&
           (left->link.transport_id == right->link.transport_id) &&
           (left->link.slot == right->link.slot) &&
           (left->link.generation == right->link.generation) &&
           (memcmp(left->session_id, right->session_id, sizeof(left->session_id)) == 0) &&
           (left->vm_epoch == right->vm_epoch) &&
           (left->input_generation == right->input_generation) &&
           (left->transition_id == right->transition_id);
}

void ev_vm_output_reset(void)
{
    uint32_t reset_generation = s_task_state.reset_generation + 1U;
    if (reset_generation == 0) {
        reset_generation = 1;
    }
    memset(&s_task_state, 0, sizeof(s_task_state));
    s_task_state.reset_generation = reset_generation;
}

void ev_vm_output_init0(void)
{
    ev_vm_output_reset();
}

bool ev_vm_output_origin_from_session(const ev_session_snapshot_t *session,
                                      ev_vm_output_origin_t *out_origin)
{
    if ((session == NULL) || (out_origin == NULL) || !session->has_owner ||
            (session->state != EV_SESSION_OWNED) || !session->handshake_complete ||
            !session->vm_ready) {
        return false;
    }

    ev_vm_output_origin_t origin = {
        .link = session->owner_link,
        .vm_epoch = session->vm_epoch,
        .input_generation = session->input_generation,
        .transition_id = session->transition_id,
    };
    memcpy(origin.session_id, session->session_id, sizeof(origin.session_id));
    if (!ev_vm_output_origin_valid(&origin)) {
        return false;
    }
    *out_origin = origin;
    return true;
}

bool ev_vm_output_scope_push(const ev_vm_output_origin_t *origin,
                             ev_vm_output_scope_t *scope)
{
    if (scope == NULL) {
        return false;
    }
    memset(scope, 0, sizeof(*scope));
    if (!ev_vm_output_origin_valid(origin) ||
            (s_task_state.scope_depth >= EV_VM_OUTPUT_SCOPE_DEPTH_MAX)) {
        return false;
    }

    uint8_t depth = s_task_state.scope_depth;
    uint32_t cookie = s_task_state.next_cookie + 1U;
    if (cookie == 0) {
        cookie = 1;
    }
    s_task_state.next_cookie = cookie;
    s_task_state.scopes[depth].cookie = cookie;
    s_task_state.scopes[depth].origin = *origin;
    s_task_state.scope_depth = (uint8_t)(depth + 1U);

    scope->cookie = cookie;
    scope->reset_generation = s_task_state.reset_generation;
    scope->depth = depth;
    scope->active = true;
    return true;
}

bool ev_vm_output_scope_can_capture_thread(void)
{
    return s_task_state.scope_depth < EV_VM_OUTPUT_SCOPE_DEPTH_MAX;
}

void ev_vm_output_scope_pop(ev_vm_output_scope_t *scope)
{
    if ((scope == NULL) || !scope->active) {
        return;
    }
    scope->active = false;
    if (scope->reset_generation != s_task_state.reset_generation) {
        return;
    }
    if ((scope->depth >= EV_VM_OUTPUT_SCOPE_DEPTH_MAX) ||
            (s_task_state.scope_depth != (uint8_t)(scope->depth + 1U)) ||
            (s_task_state.scopes[scope->depth].cookie != scope->cookie)) {
        // Preserve the current captured scope on misuse instead of exposing
        // output to the default route. VM reset clears any stranded scope.
        return;
    }

    memset(&s_task_state.scopes[scope->depth], 0,
           sizeof(s_task_state.scopes[scope->depth]));
    s_task_state.scope_depth = scope->depth;
}

bool ev_vm_output_scope_snapshot(ev_vm_output_origin_t *out_origin)
{
    if (out_origin == NULL) {
        return false;
    }
    if (s_task_state.scope_depth != 0) {
        *out_origin = s_task_state.scopes[s_task_state.scope_depth - 1U].origin;
        return true;
    }
    if (s_task_state.repl_editing && s_task_state.repl_has_origin) {
        *out_origin = s_task_state.repl_origin;
        return true;
    }
    return false;
}

void ev_vm_output_repl_read_begin(void)
{
    // A nested prompt/input operation must not replace the executing command's
    // origin. The outer execution scope remains authoritative until it returns.
    if (s_task_state.scope_depth != 0) {
        return;
    }
    memset(&s_task_state.repl_origin, 0, sizeof(s_task_state.repl_origin));
    s_task_state.repl_has_origin = false;
    s_task_state.repl_editing = true;
}

bool ev_vm_output_repl_input_accept(const ev_vm_output_origin_t *origin)
{
    if (s_task_state.scope_depth != 0) {
        const ev_vm_output_origin_t *active =
            &s_task_state.scopes[s_task_state.scope_depth - 1U].origin;
        return ev_vm_output_origin_valid(origin) &&
               ev_vm_output_origin_equal(active, origin);
    }
    if (!s_task_state.repl_editing) {
        return true;
    }
    if (!ev_vm_output_origin_valid(origin)) {
        return false;
    }
    if (!s_task_state.repl_has_origin) {
        s_task_state.repl_origin = *origin;
        s_task_state.repl_has_origin = true;
        return true;
    }
    return ev_vm_output_origin_equal(&s_task_state.repl_origin, origin);
}

bool ev_vm_output_repl_read_complete(ev_vm_output_origin_t *out_origin)
{
    bool have_origin = s_task_state.repl_editing && s_task_state.repl_has_origin;
    if (have_origin && (out_origin != NULL)) {
        *out_origin = s_task_state.repl_origin;
    }
    s_task_state.repl_editing = false;
    s_task_state.repl_has_origin = false;
    memset(&s_task_state.repl_origin, 0, sizeof(s_task_state.repl_origin));
    return have_origin && (out_origin != NULL);
}

void ev_vm_output_repl_read_abort(void)
{
    s_task_state.repl_editing = false;
    s_task_state.repl_has_origin = false;
    memset(&s_task_state.repl_origin, 0, sizeof(s_task_state.repl_origin));
}

bool ev_vm_output_repl_edit_stale(void)
{
    return s_task_state.repl_editing && s_task_state.repl_has_origin &&
           (ev_session_input_generation() != s_task_state.repl_origin.input_generation);
}

bool ev_vm_output_input_origin_stale(void)
{
    if (s_task_state.scope_depth != 0) {
        const ev_vm_output_origin_t *active =
            &s_task_state.scopes[s_task_state.scope_depth - 1U].origin;
        return ev_session_input_generation() != active->input_generation;
    }
    return ev_vm_output_repl_edit_stale();
}

bool ev_vm_output_origin_is_current(const ev_vm_output_origin_t *origin)
{
    if (!ev_vm_output_origin_valid(origin)) {
        return false;
    }
    ev_session_snapshot_t session;
    ev_vm_output_origin_t current;
    return ev_session_snapshot(&session) &&
           ev_vm_output_origin_from_session(&session, &current) &&
           ev_vm_output_origin_equal(origin, &current);
}

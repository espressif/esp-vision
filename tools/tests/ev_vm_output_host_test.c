/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ev_vm_output.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static ev_session_snapshot_t s_session;

bool ev_session_snapshot(ev_session_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return false;
    }
    *out_snapshot = s_session;
    return true;
}

uint32_t ev_session_input_generation(void)
{
    return s_session.input_generation;
}

static void make_session(ev_transport_id_t transport_id,
                         uint8_t slot,
                         uint32_t link_generation,
                         char session_fill,
                         uint32_t vm_epoch,
                         uint32_t input_generation,
                         uint32_t transition_id)
{
    memset(&s_session, 0, sizeof(s_session));
    s_session.state = EV_SESSION_OWNED;
    s_session.has_owner = true;
    s_session.owner_link.transport_id = transport_id;
    s_session.owner_link.slot = slot;
    s_session.owner_link.generation = link_generation;
    memset(s_session.session_id, session_fill, EV_SESSION_ID_HEX_LEN);
    s_session.session_id[EV_SESSION_ID_HEX_LEN] = '\0';
    s_session.handshake_complete = true;
    s_session.vm_ready = true;
    s_session.vm_epoch = vm_epoch;
    s_session.input_generation = input_generation;
    s_session.transition_id = transition_id;
}

static ev_vm_output_origin_t current_origin(void)
{
    ev_vm_output_origin_t origin;
    assert(ev_vm_output_origin_from_session(&s_session, &origin));
    return origin;
}

static bool origins_equal(const ev_vm_output_origin_t *left,
                          const ev_vm_output_origin_t *right)
{
    return (left->link.transport_id == right->link.transport_id) &&
           (left->link.slot == right->link.slot) &&
           (left->link.generation == right->link.generation) &&
           (strcmp(left->session_id, right->session_id) == 0) &&
           (left->vm_epoch == right->vm_epoch) &&
           (left->input_generation == right->input_generation) &&
           (left->transition_id == right->transition_id);
}

typedef struct {
    ev_vm_output_origin_t origin;
    ev_vm_output_origin_t observed;
    bool push_ok;
    bool snapshot_ok;
    bool current;
} child_scope_result_t;

static void *child_late_output(void *arg)
{
    child_scope_result_t *result = arg;
    ev_vm_output_scope_t scope;
    result->push_ok = ev_vm_output_scope_push(&result->origin, &scope);
    result->snapshot_ok = result->push_ok && ev_vm_output_scope_snapshot(&result->observed);
    result->current = result->push_ok && ev_vm_output_origin_is_current(&result->origin);
    if (result->push_ok) {
        ev_vm_output_scope_pop(&scope);
    }
    return NULL;
}

static void test_old_script_and_child_late_output_stays_old(void)
{
    ev_vm_output_reset();
    make_session(EV_TRANSPORT_TCP, 0, 7, 'a', 3, 11, 20);
    ev_vm_output_origin_t old_origin = current_origin();
    ev_vm_output_scope_t parent_scope;
    assert(ev_vm_output_scope_push(&old_origin, &parent_scope));
    assert(ev_vm_output_repl_input_accept(&old_origin));
    assert(!ev_vm_output_input_origin_stale());

    child_scope_result_t child = { .origin = old_origin };
    make_session(EV_TRANSPORT_USB, 0, 4, 'b', 3, 12, 21);
    ev_vm_output_origin_t new_origin = current_origin();
    assert(!ev_vm_output_origin_is_current(&old_origin));
    assert(ev_vm_output_input_origin_stale());
    assert(!ev_vm_output_repl_input_accept(&new_origin));
    assert(ev_vm_output_scope_snapshot(&child.observed));
    assert(origins_equal(&child.observed, &old_origin));

    pthread_t task;
    assert(pthread_create(&task, NULL, child_late_output, &child) == 0);
    assert(pthread_join(task, NULL) == 0);
    assert(child.push_ok && child.snapshot_ok && !child.current);
    assert(origins_equal(&child.observed, &old_origin));

    ev_vm_output_scope_pop(&parent_scope);
    assert(!ev_vm_output_scope_snapshot(&child.observed));
}

static void test_repl_generation_switch_cancels_partial_line(void)
{
    ev_vm_output_reset();
    make_session(EV_TRANSPORT_TCP, 1, 9, 'c', 5, 30, 40);
    ev_vm_output_origin_t old_origin = current_origin();
    ev_vm_output_repl_read_begin();
    assert(ev_vm_output_repl_input_accept(&old_origin));
    ev_vm_output_origin_t observed;
    assert(ev_vm_output_scope_snapshot(&observed));
    assert(origins_equal(&observed, &old_origin));

    // The control RX caller sees the changed atomic generation and returns a
    // synthetic Ctrl-C without popping the next generation's queued byte.
    make_session(EV_TRANSPORT_USB, 0, 2, 'd', 5, 31, 41);
    ev_vm_output_origin_t new_origin = current_origin();
    assert(ev_vm_output_repl_edit_stale());
    assert(ev_vm_output_input_origin_stale());
    assert(!ev_vm_output_repl_input_accept(&new_origin));
    assert(ev_vm_output_scope_snapshot(&observed));
    assert(origins_equal(&observed, &old_origin));

    ev_vm_output_repl_read_abort();
    assert(!ev_vm_output_repl_edit_stale());
    ev_vm_output_repl_read_begin();
    assert(ev_vm_output_repl_input_accept(&new_origin));
    assert(ev_vm_output_repl_read_complete(&observed));
    assert(origins_equal(&observed, &new_origin));

    ev_vm_output_scope_t scope;
    assert(ev_vm_output_scope_push(&observed, &scope));
    assert(ev_vm_output_scope_snapshot(&observed));
    assert(origins_equal(&observed, &new_origin));
    ev_vm_output_scope_pop(&scope);
}

static void test_scope_exception_and_reset_cleanup(void)
{
    ev_vm_output_reset();
    make_session(EV_TRANSPORT_USJ, 0, 3, 'e', 7, 50, 60);
    ev_vm_output_origin_t origin = current_origin();
    ev_vm_output_scope_t scope;
    assert(ev_vm_output_scope_push(&origin, &scope));
    assert(ev_vm_output_scope_snapshot(&origin));
    // Mirrors pyexec's error return: all interpreter exceptions return through
    // the wrapper, which pops its native scope before the VM handles the error.
    ev_vm_output_scope_pop(&scope);
    assert(!ev_vm_output_scope_snapshot(&origin));

    assert(ev_vm_output_scope_push(&origin, &scope));
    ev_vm_output_reset();
    ev_vm_output_scope_pop(&scope); // old reset token is inert
    assert(!ev_vm_output_scope_snapshot(&origin));
}

static void test_thread_context_capacity_fails_before_spawn(void)
{
    ev_vm_output_reset();
    make_session(EV_TRANSPORT_UART, 0, 8, 'f', 9, 70, 80);
    ev_vm_output_origin_t origin = current_origin();
    ev_vm_output_scope_t scopes[EV_VM_OUTPUT_SCOPE_DEPTH_MAX];
    for (size_t i = 0; i < EV_VM_OUTPUT_SCOPE_DEPTH_MAX; ++i) {
        assert(ev_vm_output_scope_push(&origin, &scopes[i]));
    }
    assert(!ev_vm_output_scope_can_capture_thread());
    ev_vm_output_scope_t overflow;
    assert(!ev_vm_output_scope_push(&origin, &overflow));
    for (size_t i = EV_VM_OUTPUT_SCOPE_DEPTH_MAX; i > 0; --i) {
        ev_vm_output_scope_pop(&scopes[i - 1]);
    }
    assert(ev_vm_output_scope_can_capture_thread());
}

int main(void)
{
    ev_vm_output_init0();
    test_old_script_and_child_late_output_stays_old();
    test_repl_generation_switch_cancels_partial_line();
    test_scope_exception_and_reset_cleanup();
    test_thread_context_capacity_fails_before_spawn();
    puts("ev_vm_output host tests passed");
    return 0;
}

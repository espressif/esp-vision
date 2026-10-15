/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>

#include "storage_writer_gate.h"

static void test_host_writes_and_load_are_blocked_during_lease(void)
{
    esp_vision_storage_writer_gate_t gate;
    esp_vision_storage_writer_gate_init(&gate, false, false);
    assert(esp_vision_storage_writer_host_writable(&gate));
    assert(esp_vision_storage_writer_python_io_allowed(&gate));
    assert(!esp_vision_storage_writer_begin_acquire(&gate));
    assert(esp_vision_storage_writer_begin_host_io(&gate));
    assert(esp_vision_storage_writer_finish_host_io(&gate, true));
    assert(esp_vision_storage_writer_begin_eject(&gate));
    assert(!esp_vision_storage_writer_host_writable(&gate));
    assert(!esp_vision_storage_writer_python_io_allowed(&gate));
    assert(!esp_vision_storage_writer_begin_host_io(&gate));
    assert(!esp_vision_storage_writer_begin_acquire(&gate));
    assert(esp_vision_storage_writer_complete_host_write(&gate, 1, true));
    assert(esp_vision_storage_writer_finish_eject(&gate));
    assert(esp_vision_storage_writer_python_io_allowed(&gate));

    assert(esp_vision_storage_writer_begin_acquire(&gate));
    assert(!esp_vision_storage_writer_begin_load(&gate));
    assert(esp_vision_storage_writer_finish_acquire(&gate));
    assert(gate.lease_held);
    assert(esp_vision_storage_writer_python_io_allowed(&gate));
    assert(!esp_vision_storage_writer_host_writable(&gate));
    assert(!esp_vision_storage_writer_begin_load(&gate));

    assert(esp_vision_storage_writer_begin_release(&gate));
    assert(esp_vision_storage_writer_finish_release(&gate));
    assert(gate.handoff_ready && gate.ejected && !gate.lease_held);
    assert(esp_vision_storage_writer_python_io_allowed(&gate));
    assert(!esp_vision_storage_writer_host_writable(&gate));
    assert(esp_vision_storage_writer_begin_load(&gate));
    assert(esp_vision_storage_writer_finish_load(&gate));
    assert(esp_vision_storage_writer_host_writable(&gate));
    assert(!esp_vision_storage_writer_python_io_allowed(&gate));
}

static void test_vfs_admission_is_atomic_with_transition_drain(void)
{
    esp_vision_storage_writer_gate_t gate;
    esp_vision_storage_writer_gate_init(&gate, false, false);

    /* A VFS call acquires the shared in-flight count before open/stat/etc.
     * The transition closes admission under the same serialized gate. */
    assert(esp_vision_storage_writer_begin_vfs_io(&gate));
    assert(esp_vision_storage_writer_begin_eject(&gate));
    assert(gate.transition_pending);
    assert(!esp_vision_storage_writer_begin_vfs_io(&gate));
    assert(!esp_vision_storage_writer_io_drained(&gate));
    assert(!esp_vision_storage_writer_finish_eject(&gate));

    assert(esp_vision_storage_writer_finish_vfs_io(&gate));
    assert(esp_vision_storage_writer_io_drained(&gate));
    assert(esp_vision_storage_writer_finish_eject(&gate));
    assert(esp_vision_storage_writer_begin_vfs_io(&gate));
    assert(esp_vision_storage_writer_finish_vfs_io(&gate));

    assert(esp_vision_storage_writer_begin_acquire(&gate));
    assert(!esp_vision_storage_writer_begin_vfs_io(&gate));
    assert(esp_vision_storage_writer_finish_acquire(&gate));
    assert(esp_vision_storage_writer_begin_vfs_io(&gate));
    assert(esp_vision_storage_writer_finish_vfs_io(&gate));
}

static void test_open_handle_keeps_transition_draining_after_open_call(void)
{
    esp_vision_storage_writer_gate_t gate;
    esp_vision_storage_writer_gate_init(&gate, false, false);

    /* Model open(): admission is counted before the filesystem call; after it
     * succeeds, the open-handle count is independently retained until close. */
    uint32_t open_handles = 0;
    assert(esp_vision_storage_writer_begin_vfs_io(&gate));
    open_handles++;
    assert(esp_vision_storage_writer_finish_vfs_io(&gate));
    assert(esp_vision_storage_writer_begin_eject(&gate));
    assert(esp_vision_storage_writer_io_drained(&gate));
    assert(!esp_vision_storage_writer_quiescent(&gate, open_handles));
    assert(!esp_vision_storage_writer_begin_vfs_io(&gate));
    open_handles--;
    assert(open_handles == 0);
    assert(esp_vision_storage_writer_quiescent(&gate, open_handles));
    assert(esp_vision_storage_writer_finish_eject(&gate));
}

static void test_session_loss_quarantines_without_implicit_host_reopen(void)
{
    esp_vision_storage_writer_gate_t gate;
    esp_vision_storage_writer_gate_init(&gate, false, false);
    assert(esp_vision_storage_writer_begin_eject(&gate));
    assert(esp_vision_storage_writer_finish_eject(&gate));
    assert(esp_vision_storage_writer_begin_acquire(&gate));
    assert(esp_vision_storage_writer_finish_acquire(&gate));
    esp_vision_storage_writer_session_invalidated(&gate);

    assert(gate.quarantined);
    assert(!esp_vision_storage_writer_python_io_allowed(&gate));
    assert(gate.ejected);
    assert(!gate.lease_held);
    assert(!esp_vision_storage_writer_host_writable(&gate));
    assert(!esp_vision_storage_writer_begin_load(&gate));
    assert(!esp_vision_storage_writer_begin_acquire(&gate));
}

static void test_recovered_active_is_quarantined_and_handoff_stays_ejected(void)
{
    esp_vision_storage_writer_gate_t quarantined;
    esp_vision_storage_writer_gate_init(&quarantined, true, false);
    assert(quarantined.quarantined && quarantined.ejected);
    assert(!esp_vision_storage_writer_begin_load(&quarantined));

    esp_vision_storage_writer_gate_t handoff;
    esp_vision_storage_writer_gate_init(&handoff, false, true);
    assert(handoff.handoff_ready && handoff.ejected);
    assert(esp_vision_storage_writer_begin_load(&handoff));
    assert(esp_vision_storage_writer_finish_load(&handoff));
    assert(esp_vision_storage_writer_host_writable(&handoff));
}

static void test_writer_underflow_or_overflow_fails_closed(void)
{
    esp_vision_storage_writer_gate_t gate;
    esp_vision_storage_writer_gate_init(&gate, false, false);
    assert(!esp_vision_storage_writer_finish_host_io(&gate, false));
    assert(gate.quarantined);

    esp_vision_storage_writer_gate_init(&gate, false, false);
    assert(esp_vision_storage_writer_begin_host_io(&gate));
    gate.in_flight_io = UINT32_MAX;
    assert(!esp_vision_storage_writer_begin_host_io(&gate));
}

static void test_command_sync_covers_all_chunks_without_clearing_newer_writes(void)
{
    esp_vision_storage_writer_gate_t gate;
    esp_vision_storage_writer_gate_init(&gate, false, false);

    for (unsigned int i = 0; i < 3; i++) {
        assert(esp_vision_storage_writer_begin_host_io(&gate));
        assert(esp_vision_storage_writer_finish_host_io(&gate, true));
    }
    assert(gate.pending_write_sync == 3);
    assert(esp_vision_storage_writer_complete_host_write(&gate, 3, true));
    assert(gate.pending_write_sync == 0);
    assert(esp_vision_storage_writer_io_drained(&gate));

    for (unsigned int i = 0; i < 3; i++) {
        assert(esp_vision_storage_writer_begin_host_io(&gate));
        assert(esp_vision_storage_writer_finish_host_io(&gate, true));
    }
    /* Model a later chunk accepted while the prior command's sync is in
     * flight. Completing the older sequence must not clear the newer chunk. */
    assert(esp_vision_storage_writer_begin_host_io(&gate));
    assert(esp_vision_storage_writer_finish_host_io(&gate, true));
    assert(esp_vision_storage_writer_complete_host_write(&gate, 3, true));
    assert(gate.pending_write_sync == 4);
    assert(esp_vision_storage_writer_complete_host_write(&gate, 4, true));
    assert(gate.pending_write_sync == 0);

    /* TinyUSB may report completion for a zero-data or rejected command. It
     * must not underflow or quarantine a clean volume. */
    assert(esp_vision_storage_writer_complete_host_write(&gate, 0, false));
    assert(!gate.quarantined);

    assert(esp_vision_storage_writer_begin_host_io(&gate));
    assert(esp_vision_storage_writer_finish_host_io(&gate, true));
    assert(!esp_vision_storage_writer_complete_host_write(&gate, 1, false));
    assert(gate.quarantined);
}

int main(void)
{
    test_host_writes_and_load_are_blocked_during_lease();
    test_vfs_admission_is_atomic_with_transition_drain();
    test_open_handle_keeps_transition_draining_after_open_call();
    test_session_loss_quarantines_without_implicit_host_reopen();
    test_recovered_active_is_quarantined_and_handoff_stays_ejected();
    test_writer_underflow_or_overflow_fails_closed();
    test_command_sync_covers_all_chunks_without_clearing_newer_writes();
    return 0;
}

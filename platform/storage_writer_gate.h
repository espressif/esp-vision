/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_STORAGE_WRITER_GATE_H
#define ESP_VISION_STORAGE_WRITER_GATE_H

#include <stdbool.h>
#include <stdint.h>

/* Portable per-volume gate shared by firmware callbacks and native fault tests.
 * Callers serialize access (the firmware uses a portMUX). */
typedef struct {
    bool ejected;
    bool transition_pending;
    bool lease_held;
    bool handoff_ready;
    bool managed;
    bool quarantined;
    uint32_t in_flight_io;
    uint32_t pending_write_sync;
} esp_vision_storage_writer_gate_t;

static inline void esp_vision_storage_writer_gate_init(esp_vision_storage_writer_gate_t *gate,
                                                       bool recovered_quarantine,
                                                       bool recovered_handoff)
{
    gate->ejected = recovered_quarantine || recovered_handoff;
    gate->transition_pending = false;
    gate->lease_held = false;
    gate->handoff_ready = recovered_handoff;
    gate->managed = recovered_quarantine || recovered_handoff;
    gate->quarantined = recovered_quarantine;
    gate->in_flight_io = 0;
    gate->pending_write_sync = 0;
}

static inline bool esp_vision_storage_writer_host_writable(const esp_vision_storage_writer_gate_t *gate)
{
    return !gate->ejected && !gate->transition_pending &&
           !gate->lease_held && !gate->quarantined;
}

static inline bool esp_vision_storage_writer_host_load_allowed(const esp_vision_storage_writer_gate_t *gate)
{
    return !gate->transition_pending && !gate->lease_held && !gate->quarantined;
}

/* Preserve the product's existing MicroPython VFS behavior outside managed
 * handoff transitions. The Agent must not infer a lease from this legacy mode. */
static inline bool esp_vision_storage_writer_python_io_allowed(const esp_vision_storage_writer_gate_t *gate)
{
    return !gate->transition_pending && !gate->quarantined &&
           (!gate->managed || gate->handoff_ready || gate->lease_held);
}

static inline bool esp_vision_storage_writer_begin_host_io(esp_vision_storage_writer_gate_t *gate)
{
    if (!esp_vision_storage_writer_host_writable(gate) || gate->in_flight_io == UINT32_MAX) {
        return false;
    }
    gate->in_flight_io++;
    return true;
}

static inline bool esp_vision_storage_writer_begin_vfs_io(esp_vision_storage_writer_gate_t *gate)
{
    if (!esp_vision_storage_writer_python_io_allowed(gate) || gate->in_flight_io == UINT32_MAX) {
        return false;
    }
    gate->in_flight_io++;
    return true;
}

static inline bool esp_vision_storage_writer_finish_vfs_io(esp_vision_storage_writer_gate_t *gate)
{
    if (gate->in_flight_io == 0) {
        gate->quarantined = true;
        gate->ejected = true;
        return false;
    }
    gate->in_flight_io--;
    return true;
}

static inline bool esp_vision_storage_writer_finish_host_io(esp_vision_storage_writer_gate_t *gate,
                                                            bool accepted_write)
{
    if (gate->in_flight_io == 0 ||
            (accepted_write && gate->pending_write_sync == UINT32_MAX)) {
        gate->quarantined = true;
        gate->ejected = true;
        return false;
    }
    gate->in_flight_io--;
    if (accepted_write) {
        gate->pending_write_sync++;
    }
    return true;
}

static inline bool esp_vision_storage_writer_complete_host_write(esp_vision_storage_writer_gate_t *gate,
                                                                 uint32_t sync_sequence,
                                                                 bool sync_succeeded)
{
    /* TinyUSB calls write10_complete once for the whole SCSI command, whereas
     * write10_cb may run once for each data chunk. A zero sequence means the
     * command accepted no writes and is a harmless no-op. */
    if (sync_sequence == 0) {
        return true;
    }
    if (!sync_succeeded || gate->pending_write_sync < sync_sequence) {
        gate->quarantined = true;
        gate->ejected = true;
        return false;
    }
    /* If another chunk arrived while sync was in progress, keep it pending
     * for its own command completion. Never clear writes newer than the
     * sequence that this sync covered. */
    if (gate->pending_write_sync == sync_sequence) {
        gate->pending_write_sync = 0;
    }
    return true;
}

static inline bool esp_vision_storage_writer_io_drained(const esp_vision_storage_writer_gate_t *gate)
{
    return gate->in_flight_io == 0 && gate->pending_write_sync == 0;
}

static inline bool esp_vision_storage_writer_quiescent(const esp_vision_storage_writer_gate_t *gate,
                                                       uint32_t open_handles)
{
    return esp_vision_storage_writer_io_drained(gate) && open_handles == 0;
}

static inline bool esp_vision_storage_writer_begin_eject(esp_vision_storage_writer_gate_t *gate)
{
    if (gate->transition_pending || gate->lease_held || gate->quarantined) {
        return false;
    }
    gate->ejected = true;
    gate->transition_pending = true;
    return true;
}

static inline bool esp_vision_storage_writer_finish_eject(esp_vision_storage_writer_gate_t *gate)
{
    if (!gate->transition_pending || gate->lease_held || gate->quarantined ||
            !esp_vision_storage_writer_io_drained(gate)) {
        return false;
    }
    gate->transition_pending = false;
    gate->handoff_ready = true;
    gate->ejected = true;
    return true;
}

static inline bool esp_vision_storage_writer_begin_load(esp_vision_storage_writer_gate_t *gate)
{
    if (!gate->ejected || !gate->handoff_ready || !esp_vision_storage_writer_host_load_allowed(gate) ||
            !esp_vision_storage_writer_io_drained(gate)) {
        return false;
    }
    gate->transition_pending = true;
    return true;
}

static inline bool esp_vision_storage_writer_finish_load(esp_vision_storage_writer_gate_t *gate)
{
    if (!gate->transition_pending || !gate->ejected || gate->lease_held || gate->quarantined ||
            !esp_vision_storage_writer_io_drained(gate)) {
        return false;
    }
    gate->transition_pending = false;
    gate->ejected = false;
    gate->handoff_ready = false;
    return true;
}

static inline bool esp_vision_storage_writer_begin_acquire(esp_vision_storage_writer_gate_t *gate)
{
    if (!gate->ejected || gate->transition_pending || gate->lease_held || gate->quarantined) {
        return false;
    }
    gate->transition_pending = true;
    return true;
}

static inline bool esp_vision_storage_writer_finish_acquire(esp_vision_storage_writer_gate_t *gate)
{
    if (!gate->transition_pending || !esp_vision_storage_writer_io_drained(gate)) {
        return false;
    }
    gate->transition_pending = false;
    gate->lease_held = true;
    gate->handoff_ready = false;
    gate->managed = true;
    gate->ejected = true;
    return true;
}

static inline bool esp_vision_storage_writer_begin_release(esp_vision_storage_writer_gate_t *gate)
{
    if (!gate->lease_held || gate->transition_pending || gate->quarantined) {
        return false;
    }
    gate->transition_pending = true;
    return true;
}

static inline bool esp_vision_storage_writer_finish_release(esp_vision_storage_writer_gate_t *gate)
{
    if (!gate->lease_held || !gate->transition_pending ||
            !esp_vision_storage_writer_io_drained(gate)) {
        return false;
    }
    gate->lease_held = false;
    gate->transition_pending = false;
    gate->handoff_ready = true;
    gate->managed = true;
    gate->ejected = true;
    return true;
}

static inline void esp_vision_storage_writer_quarantine(esp_vision_storage_writer_gate_t *gate)
{
    gate->transition_pending = false;
    gate->lease_held = false;
    gate->managed = true;
    gate->quarantined = true;
    gate->ejected = true;
}

static inline void esp_vision_storage_writer_session_invalidated(esp_vision_storage_writer_gate_t *gate)
{
    if (gate->transition_pending || gate->lease_held) {
        esp_vision_storage_writer_quarantine(gate);
    }
}

#endif /* ESP_VISION_STORAGE_WRITER_GATE_H */

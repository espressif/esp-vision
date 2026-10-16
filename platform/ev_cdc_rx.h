/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EV_CDC_RX_RING_CAPACITY (2048)

typedef void (*ev_cdc_rx_flush_fn_t)(void *context);

typedef struct {
    uint32_t generation;
    bool mounted;
    bool mount_seen;
    bool dtr;
    bool valid;
} ev_cdc_rx_state_t;

typedef struct {
    bool changed;
    bool fell;
    bool rose;
    bool valid;
    uint32_t generation;
} ev_cdc_rx_event_t;

static inline void ev_cdc_rx_advance_generation(ev_cdc_rx_state_t *state)
{
    if (state->valid) {
        if (state->generation == UINT32_MAX) {
            state->valid = false;
        } else {
            ++state->generation;
        }
    }
}

static inline ev_cdc_rx_event_t ev_cdc_rx_bus_mount(ev_cdc_rx_state_t *state,
                                                    ev_cdc_rx_flush_fn_t flush,
                                                    void *context)
{
    ev_cdc_rx_event_t event = {0};
    if (state == NULL) {
        return event;
    }

    event.changed = true;
    event.rose = true;
    if (state->mount_seen) {
        // A configuration mount can follow a USB bus reset without an
        // intervening umount callback. Retire input on every later mount.
        ev_cdc_rx_advance_generation(state);
    }
    state->mount_seen = true;
    state->mounted = true;
    if (flush != NULL) {
        flush(context);
    }

    event.valid = state->valid;
    event.generation = state->generation;
    return event;
}

static inline ev_cdc_rx_event_t ev_cdc_rx_bus_unmount(ev_cdc_rx_state_t *state,
                                                      ev_cdc_rx_flush_fn_t flush,
                                                      void *context)
{
    ev_cdc_rx_event_t event = {0};
    if (state == NULL) {
        return event;
    }

    event.changed = state->mounted;
    event.fell = event.changed;
    if (event.changed) {
        ev_cdc_rx_advance_generation(state);
    }
    state->mounted = false;
    if (flush != NULL) {
        flush(context);
    }

    event.valid = state->valid;
    event.generation = state->generation;
    return event;
}

// Synchronize a task-side snapshot with mount/unmount callbacks. The TinyUSB
// callbacks remain the authoritative boundary, including a reset/reconfigure
// that completes between two transport polls.
static inline ev_cdc_rx_event_t ev_cdc_rx_bus_state(ev_cdc_rx_state_t *state,
                                                    bool mounted,
                                                    ev_cdc_rx_flush_fn_t flush,
                                                    void *context)
{
    if (state == NULL) {
        return (ev_cdc_rx_event_t) {
            0
        };
    }
    if (mounted == state->mounted) {
        return (ev_cdc_rx_event_t) {
            .valid = state->valid,
            .generation = state->generation,
        };
    }
    return mounted ? ev_cdc_rx_bus_mount(state, flush, context) :
           ev_cdc_rx_bus_unmount(state, flush, context);
}

// DTR is retained only to preserve raw-REPL close behavior. Its edges never
// change EV-MUX readiness or the physical RX generation.
static inline bool ev_cdc_rx_dtr_update(ev_cdc_rx_state_t *state, bool dtr)
{
    if (state == NULL) {
        return false;
    }
    bool fell = state->dtr && !dtr;
    state->dtr = dtr;
    return fell;
}

// A transport failure can retire a logical link while the USB bus remains
// mounted. Start a fresh RX generation and discard buffered input for it.
static inline bool ev_cdc_rx_restart(ev_cdc_rx_state_t *state,
                                     ev_cdc_rx_flush_fn_t flush,
                                     void *context)
{
    if ((state == NULL) || !state->valid || (state->generation == UINT32_MAX)) {
        if (state != NULL) {
            state->valid = false;
        }
        return false;
    }
    ++state->generation;
    if (flush != NULL) {
        flush(context);
    }
    return true;
}

static inline uint16_t ev_cdc_rx_ring_next(uint16_t index, uint32_t size)
{
    uint32_t next = (uint32_t)index + 1;
    return (uint16_t)(next == size ? 0 : next);
}

// Put one byte and its generation tag into a ringbuf-compatible circular ring.
static inline bool ev_cdc_rx_ring_put(uint8_t *buffer,
                                      uint32_t *tags,
                                      uint32_t size,
                                      uint16_t *iget,
                                      uint16_t *iput,
                                      uint8_t value,
                                      uint32_t generation)
{
    if ((buffer == NULL) || (tags == NULL) || (iget == NULL) || (iput == NULL) ||
            (size < 2) || (generation == 0) || (*iget >= size) || (*iput >= size)) {
        return false;
    }
    uint16_t next = ev_cdc_rx_ring_next(*iput, size);
    if (next == *iget) {
        return false;
    }
    buffer[*iput] = value;
    tags[*iput] = generation;
    *iput = next;
    return true;
}

// Keep only bytes tagged with the current connection generation. This works
// in-place, preserves ring order, and leaves the shared ring's read index intact.
static inline size_t ev_cdc_rx_ring_filter(uint8_t *buffer,
                                           uint32_t *tags,
                                           uint32_t size,
                                           uint16_t *iget,
                                           uint16_t *iput,
                                           bool keep_generation,
                                           uint32_t generation)
{
    if ((buffer == NULL) || (tags == NULL) || (iget == NULL) || (iput == NULL) ||
            (size < 2) || (*iget >= size) || (*iput >= size) ||
            (keep_generation && generation == 0)) {
        return 0;
    }

    uint16_t read = *iget;
    uint16_t write = read;
    uint16_t end = *iput;
    size_t kept = 0;
    while (read != end) {
        uint16_t next = ev_cdc_rx_ring_next(read, size);
        if (keep_generation && tags[read] == generation) {
            if (write != read) {
                buffer[write] = buffer[read];
                tags[write] = tags[read];
            }
            write = ev_cdc_rx_ring_next(write, size);
            ++kept;
        }
        read = next;
    }
    *iput = write;
    return kept;
}

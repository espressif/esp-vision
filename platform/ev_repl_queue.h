/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EV_REPL_QUEUE_CAPACITY (512)

typedef struct {
    uint8_t value;
    uint32_t generation;
} ev_repl_queue_entry_t;

typedef struct {
    ev_repl_queue_entry_t entries[EV_REPL_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    uint32_t guard_generation;
    bool guard_valid;
} ev_repl_queue_t;

static inline void ev_repl_queue_force_clear(ev_repl_queue_t *queue, uint32_t generation)
{
    if (queue == NULL) {
        return;
    }
    queue->head = 0;
    queue->tail = 0;
    queue->guard_generation = generation;
    queue->guard_valid = generation != 0;
}

// A delayed callback may clear only the generation it observed. If a producer
// already installed the live generation, preserve its newly queued input.
static inline bool ev_repl_queue_clear_generation(ev_repl_queue_t *queue,
                                                  uint32_t generation,
                                                  uint32_t current_generation)
{
    if ((queue == NULL) || (generation == 0) || (generation != current_generation)) {
        return false;
    }
    if (!queue->guard_valid || (queue->guard_generation != generation)) {
        ev_repl_queue_force_clear(queue, generation);
    }
    return true;
}

static inline bool ev_repl_queue_push(ev_repl_queue_t *queue,
                                      const uint8_t *data,
                                      size_t length,
                                      uint32_t generation,
                                      uint32_t current_generation)
{
    if ((queue == NULL) || ((length != 0) && (data == NULL)) ||
            (generation == 0) || (generation != current_generation)) {
        return false;
    }
    (void)ev_repl_queue_clear_generation(queue, generation, current_generation);
    size_t free_bytes = (queue->head + EV_REPL_QUEUE_CAPACITY - queue->tail - 1) %
                        EV_REPL_QUEUE_CAPACITY;
    if (length > free_bytes) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        queue->entries[queue->tail].value = data[i];
        queue->entries[queue->tail].generation = generation;
        queue->tail = (queue->tail + 1) % EV_REPL_QUEUE_CAPACITY;
    }
    return true;
}

static inline bool ev_repl_queue_pop(ev_repl_queue_t *queue,
                                     uint32_t generation,
                                     uint32_t current_generation,
                                     bool consumer_ready,
                                     uint8_t *out_value)
{
    if ((queue == NULL) || (out_value == NULL) || (generation == 0) ||
            (generation != current_generation)) {
        return false;
    }
    if (!consumer_ready) {
        ev_repl_queue_force_clear(queue, generation);
        return false;
    }
    (void)ev_repl_queue_clear_generation(queue, generation, current_generation);
    while (queue->head != queue->tail) {
        ev_repl_queue_entry_t *entry = &queue->entries[queue->head];
        queue->head = (queue->head + 1) % EV_REPL_QUEUE_CAPACITY;
        if (entry->generation == generation) {
            *out_value = entry->value;
            return true;
        }
    }
    return false;
}

static inline bool ev_repl_queue_pending(ev_repl_queue_t *queue,
                                         uint32_t generation,
                                         uint32_t current_generation,
                                         bool consumer_ready)
{
    if ((queue == NULL) || (generation == 0) || (generation != current_generation)) {
        return false;
    }
    if (!consumer_ready) {
        ev_repl_queue_force_clear(queue, generation);
        return false;
    }
    (void)ev_repl_queue_clear_generation(queue, generation, current_generation);
    while (queue->head != queue->tail) {
        ev_repl_queue_entry_t *entry = &queue->entries[queue->head];
        if (entry->generation == generation) {
            return true;
        }
        queue->head = (queue->head + 1) % EV_REPL_QUEUE_CAPACITY;
    }
    return false;
}

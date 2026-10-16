/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ev_cdc_rx.h"

typedef struct {
    uint8_t bytes[32];
    size_t length;
    bool pending;
    size_t flush_count;
    uint16_t *iget;
    uint16_t *iput;
} fake_cdc_fifo_t;

static void fake_cdc_flush(void *context)
{
    fake_cdc_fifo_t *fifo = (fake_cdc_fifo_t *)context;
    fifo->length = 0;
    fifo->pending = false;
    if ((fifo->iget != NULL) && (fifo->iput != NULL)) {
        *fifo->iget = *fifo->iput = 0;
    }
    ++fifo->flush_count;
}

static void fake_cdc_fifo_append(fake_cdc_fifo_t *fifo, const uint8_t *data, size_t length)
{
    assert(length <= sizeof(fifo->bytes) - fifo->length);
    memcpy(fifo->bytes + fifo->length, data, length);
    fifo->length += length;
    fifo->pending = true;
}

static void fake_cdc_ring_receive(fake_cdc_fifo_t *fifo,
                                  ev_cdc_rx_state_t *state,
                                  uint8_t *ring,
                                  uint32_t *tags,
                                  uint32_t ring_size,
                                  uint16_t *iget,
                                  uint16_t *iput)
{
    for (size_t i = 0; i < fifo->length; ++i) {
        assert(ev_cdc_rx_ring_put(ring, tags, ring_size, iget, iput,
                                  fifo->bytes[i], state->generation));
    }
    fifo->length = 0;
    fifo->pending = false;
}

static void test_mount_keeps_first_packet_after_generation_boundary(void)
{
    ev_cdc_rx_state_t state = {.generation = 1, .valid = true};
    uint8_t ring[16] = {0};
    uint32_t tags[16] = {0};
    uint16_t iget = 0;
    uint16_t iput = 0;
    fake_cdc_fifo_t fifo = {
        .bytes = {0xaa, 0xbb}, .length = 2, .pending = true,
        .iget = &iget, .iput = &iput,
    };
    static const uint8_t capabilities[] = {0x7e, 'c', 'a', 'p', 0x7f};

    assert(ev_cdc_rx_ring_put(ring, tags, sizeof(ring), &iget, &iput,
                              'o', state.generation));
    ev_cdc_rx_event_t mounted = ev_cdc_rx_bus_mount(&state, fake_cdc_flush, &fifo);
    assert(mounted.changed && mounted.rose && mounted.valid);
    assert(mounted.generation == 1 && state.mounted);
    assert(fifo.length == 0 && !fifo.pending && iget == iput);

    fake_cdc_fifo_append(&fifo, capabilities, sizeof(capabilities));
    fake_cdc_ring_receive(&fifo, &state, ring, tags, sizeof(ring), &iget, &iput);
    assert(ev_cdc_rx_ring_filter(ring, tags, sizeof(ring), &iget, &iput,
                                 true, state.generation) == sizeof(capabilities));
    assert(iput == sizeof(capabilities));
    assert(memcmp(ring, capabilities, sizeof(capabilities)) == 0);
}

static void test_dtr_edges_do_not_change_generation_or_mount(void)
{
    ev_cdc_rx_state_t state = {.generation = 9, .valid = true};
    fake_cdc_fifo_t fifo = {0};
    ev_cdc_rx_event_t mounted = ev_cdc_rx_bus_mount(&state, fake_cdc_flush, &fifo);
    assert(mounted.valid && state.mounted);
    uint32_t generation = state.generation;

    assert(!ev_cdc_rx_dtr_update(&state, true));
    assert(!ev_cdc_rx_dtr_update(&state, true));
    assert(ev_cdc_rx_dtr_update(&state, false));
    assert(!ev_cdc_rx_dtr_update(&state, false));
    assert(state.mounted && state.valid && state.generation == generation);
    assert(fifo.flush_count == 1);
}

static void test_disconnect_and_fast_reopen_isolate_old_fifo_and_ring(void)
{
    ev_cdc_rx_state_t state = {.generation = 8, .valid = true};
    uint8_t ring[16] = {0};
    uint32_t tags[16] = {0};
    uint16_t iget = 0;
    uint16_t iput = 0;
    fake_cdc_fifo_t fifo = {
        .bytes = {0xaa, 0xbb, 0xcc}, .length = 3, .pending = true,
        .iget = &iget, .iput = &iput,
    };
    static const uint8_t old_bytes[] = {'o', 'l', 'd'};
    static const uint8_t capabilities[] = {0x7e, 'c', 'a', 'p', 0x7f};

    (void)ev_cdc_rx_bus_mount(&state, fake_cdc_flush, &fifo);
    for (size_t i = 0; i < sizeof(old_bytes); ++i) {
        assert(ev_cdc_rx_ring_put(ring, tags, sizeof(ring), &iget, &iput,
                                  old_bytes[i], state.generation));
    }
    ev_cdc_rx_event_t unmounted = ev_cdc_rx_bus_unmount(&state, fake_cdc_flush, &fifo);
    assert(unmounted.changed && unmounted.fell && unmounted.valid);
    assert(unmounted.generation == 9 && !state.mounted);
    assert(fifo.length == 0 && !fifo.pending && iget == iput);

    // A quick reconnect is a distinct generation. The mount callback runs
    // before TinyUSB can queue the new host's first bulk packet.
    ev_cdc_rx_event_t mounted = ev_cdc_rx_bus_mount(&state, fake_cdc_flush, &fifo);
    assert(mounted.changed && mounted.rose && mounted.valid);
    assert(mounted.generation == 10 && state.mounted);
    fake_cdc_fifo_append(&fifo, capabilities, sizeof(capabilities));
    fake_cdc_ring_receive(&fifo, &state, ring, tags, sizeof(ring), &iget, &iput);
    assert(ev_cdc_rx_ring_filter(ring, tags, sizeof(ring), &iget, &iput,
                                 true, state.generation) == sizeof(capabilities));
    assert((iput + sizeof(ring) - iget) % sizeof(ring) == sizeof(capabilities));
    for (size_t i = 0; i < sizeof(capabilities); ++i) {
        assert(ring[(iget + i) % sizeof(ring)] == capabilities[i]);
    }
}

static void test_reset_reconfigure_mount_retires_old_input_without_umount(void)
{
    ev_cdc_rx_state_t state = {.generation = 3, .valid = true};
    uint8_t ring[8] = {0};
    uint32_t tags[8] = {0};
    uint16_t iget = 0;
    uint16_t iput = 0;
    fake_cdc_fifo_t fifo = {.iget = &iget, .iput = &iput};
    static const uint8_t new_frame[] = {0x7e, 'n', 0x7f};

    (void)ev_cdc_rx_bus_mount(&state, fake_cdc_flush, &fifo);
    assert(ev_cdc_rx_ring_put(ring, tags, sizeof(ring), &iget, &iput,
                              'x', state.generation));

    // TinyUSB's bus-reset event clears configuration without calling its
    // application umount callback. A later configuration mount still retires
    // the old ring generation before any new packet is delivered.
    ev_cdc_rx_event_t remounted = ev_cdc_rx_bus_mount(&state, fake_cdc_flush, &fifo);
    assert(remounted.changed && remounted.valid && remounted.generation == 4);
    assert(iget == iput);
    for (size_t i = 0; i < sizeof(new_frame); ++i) {
        assert(ev_cdc_rx_ring_put(ring, tags, sizeof(ring), &iget, &iput,
                                  new_frame[i], state.generation));
    }
    assert(ev_cdc_rx_ring_filter(ring, tags, sizeof(ring), &iget, &iput,
                                 true, state.generation) == sizeof(new_frame));
}

static void test_ring_filter_wrap_and_shared_consumer(void)
{
    uint8_t ring[8] = {0};
    uint32_t tags[8] = {0};
    uint16_t iget = 6;
    uint16_t iput = 2;
    ring[6] = 'a'; tags[6] = 1;
    ring[7] = 'b'; tags[7] = 2;
    ring[0] = 'c'; tags[0] = 2;
    ring[1] = 'd'; tags[1] = 1;
    assert(ev_cdc_rx_ring_filter(ring, tags, sizeof(ring), &iget, &iput, true, 2) == 2);
    assert(iget == 6 && iput == 0);
    assert(ring[6] == 'b' && ring[7] == 'c');

    // Raw REPL advances the same ring's read index without touching tags.
    iget = ev_cdc_rx_ring_next(iget, sizeof(ring));
    assert(ev_cdc_rx_ring_put(ring, tags, sizeof(ring), &iget, &iput, 'n', 3));
    assert(ev_cdc_rx_ring_filter(ring, tags, sizeof(ring), &iget, &iput, true, 3) == 1);
    assert(ring[7] == 'n' && iput == 0);
}

static void test_generation_exhaustion_never_wraps(void)
{
    ev_cdc_rx_state_t state = {
        .generation = UINT32_MAX, .mounted = true, .mount_seen = true, .valid = true,
    };
    fake_cdc_fifo_t fifo = {.bytes = {0x55}, .length = 1, .pending = true};
    ev_cdc_rx_event_t unmounted = ev_cdc_rx_bus_unmount(&state, fake_cdc_flush, &fifo);
    assert(unmounted.changed && unmounted.fell && !unmounted.valid);
    assert(unmounted.generation == UINT32_MAX && state.generation == UINT32_MAX);
    assert(fifo.length == 0 && !fifo.pending);
    assert(!ev_cdc_rx_restart(&state, fake_cdc_flush, &fifo));
}

int main(void)
{
    assert(EV_CDC_RX_RING_CAPACITY == 2048);
    test_mount_keeps_first_packet_after_generation_boundary();
    test_dtr_edges_do_not_change_generation_or_mount();
    test_disconnect_and_fast_reopen_isolate_old_fifo_and_ring();
    test_reset_reconfigure_mount_retires_old_input_without_umount();
    test_ring_filter_wrap_and_shared_consumer();
    test_generation_exhaustion_never_wraps();
    return 0;
}

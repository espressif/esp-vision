/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ev_channel.h"
#include "py/mphal.h"

static ev_link_ref_t s_live_links[EV_TRANSPORT_MAX][EV_TRANSPORT_LINK_SLOTS_MAX];
static bool s_link_open[EV_TRANSPORT_MAX][EV_TRANSPORT_LINK_SLOTS_MAX];

bool ev_transport_id_from_name(const char *name, ev_transport_id_t *out_id)
{
    static const char *const names[] = { "usj", "usb", "uart", "tcp" };
    if ((name == NULL) || (out_id == NULL)) {
        return false;
    }
    for (ev_transport_id_t id = EV_TRANSPORT_USJ; id < EV_TRANSPORT_MAX; ++id) {
        if (strcmp(name, names[id]) == 0) {
            *out_id = id;
            return true;
        }
    }
    return false;
}

const char *ev_transport_name(ev_transport_id_t id)
{
    static const char *const names[] = { "usj", "usb", "uart", "tcp" };
    return id < EV_TRANSPORT_MAX ? names[id] : "unknown";
}

bool ev_transport_link_ref(ev_transport_id_t id, uint8_t slot, ev_link_ref_t *out_link)
{
    if ((out_link == NULL) || (id >= EV_TRANSPORT_MAX) ||
            (slot >= EV_TRANSPORT_LINK_SLOTS_MAX) || !s_link_open[id][slot]) {
        return false;
    }
    *out_link = s_live_links[id][slot];
    return true;
}

bool ev_transport_link_is_current(const ev_link_ref_t *link)
{
    if ((link == NULL) || (link->transport_id >= EV_TRANSPORT_MAX) ||
            (link->slot >= EV_TRANSPORT_LINK_SLOTS_MAX) ||
            !s_link_open[link->transport_id][link->slot]) {
        return false;
    }
    const ev_link_ref_t *live = &s_live_links[link->transport_id][link->slot];
    return (live->transport_id == link->transport_id) &&
           (live->slot == link->slot) && (live->generation == link->generation);
}

void ev_transport_set_owner(const ev_link_ref_t *owner_or_null)
{
    (void)owner_or_null;
}

ssize_t mp_hal_stdout_tx_strn(const char *str, mp_uint_t len)
{
    (void)str;
    return (ssize_t)len;
}

int main(void)
{
    const ev_link_ref_t tcp_owner = {
        .transport_id = EV_TRANSPORT_TCP,
        .slot = 1,
        .generation = 17,
    };
    const ev_link_ref_t usj_fallback = {
        .transport_id = EV_TRANSPORT_USJ,
        .slot = EV_TRANSPORT_PHYSICAL_SLOT,
        .generation = 4,
    };
    s_live_links[tcp_owner.transport_id][tcp_owner.slot] = tcp_owner;
    s_link_open[tcp_owner.transport_id][tcp_owner.slot] = true;
    s_live_links[usj_fallback.transport_id][usj_fallback.slot] = usj_fallback;
    s_link_open[usj_fallback.transport_id][usj_fallback.slot] = true;

    ev_channel_init0();
    assert(ev_channel_set_owner_link(&tcp_owner) == ESP_OK);
    for (ev_stream_t stream = EV_STREAM_USER; stream < EV_STREAM_MAX; ++stream) {
        ev_link_ref_t target;
        assert(strcmp(ev_channel_get(stream), "tcp") == 0);
        assert(ev_channel_target_link(stream, &target));
        assert(target.transport_id == tcp_owner.transport_id);
        assert(target.slot == tcp_owner.slot);
        assert(target.generation == tcp_owner.generation);
    }

    // A second init models the preview/VM initialization path across soft reset.
    ev_channel_init0();
    for (ev_stream_t stream = EV_STREAM_USER; stream < EV_STREAM_MAX; ++stream) {
        ev_link_ref_t target;
        assert(strcmp(ev_channel_get(stream), "tcp") == 0);
        assert(ev_channel_target_link(stream, &target));
        assert(target.transport_id == tcp_owner.transport_id);
        assert(target.slot == tcp_owner.slot);
        assert(target.generation == tcp_owner.generation);
    }

    assert(ev_channel_set_owner_link(NULL) == ESP_OK);
    for (ev_stream_t stream = EV_STREAM_USER; stream < EV_STREAM_MAX; ++stream) {
        ev_link_ref_t target;
        assert(strcmp(ev_channel_get(stream), "usj") == 0);
        assert(ev_channel_target_link(stream, &target));
        assert(target.transport_id == usj_fallback.transport_id);
        assert(target.slot == usj_fallback.slot);
        assert(target.generation == usj_fallback.generation);
    }

    puts("channel owner survives repeated init and release restores USJ fallback");
    return 0;
}

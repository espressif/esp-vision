/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ev_session.h"
#include "ev_channel.h"
#include "ev_repl_queue.h"

static uint32_t generations[EV_TRANSPORT_MAX][EV_TRANSPORT_LINK_SLOTS_MAX];
static bool opened[EV_TRANSPORT_MAX][EV_TRANSPORT_LINK_SLOTS_MAX];
static ev_link_ref_t transport_owner;
static bool has_transport_owner;
static ev_link_ref_t channel_owner;
static bool has_channel_owner;
static uint32_t random_value = 0x12345678;
static int64_t timer_us;

static pthread_mutex_t route_test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t route_test_condition = PTHREAD_COND_INITIALIZER;
static bool block_fallback_route;
static bool fallback_route_entered;
static bool allow_fallback_route;

static bool link_equal(const ev_link_ref_t *a, const ev_link_ref_t *b)
{
    return a->transport_id == b->transport_id && a->slot == b->slot &&
           a->generation == b->generation;
}

static ev_link_ref_t current_link(ev_transport_id_t id, uint8_t slot)
{
    assert(opened[id][slot]);
    return (ev_link_ref_t) {
        .transport_id = id, .slot = slot, .generation = generations[id][slot]
    };
}

uint32_t esp_random(void)
{
    random_value = random_value * 1664525u + 1013904223u;
    return random_value;
}

int64_t esp_timer_get_time(void)
{
    return timer_us;
}

bool ev_transport_id_from_name(const char *name, ev_transport_id_t *out_id)
{
    static const char *const names[] = { "usj", "usb", "uart", "tcp" };
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

bool ev_transport_link_is_current(const ev_link_ref_t *link)
{
    return link != NULL && link->transport_id < EV_TRANSPORT_MAX &&
           link->slot < EV_TRANSPORT_LINK_SLOTS_MAX && opened[link->transport_id][link->slot] &&
           generations[link->transport_id][link->slot] == link->generation;
}

bool ev_transport_link_advance_generation(const ev_link_ref_t *link, ev_link_ref_t *out_link)
{
    if (!ev_transport_link_is_current(link)) {
        return false;
    }
    uint32_t next = generations[link->transport_id][link->slot] + 1;
    generations[link->transport_id][link->slot] = next == 0 ? 1 : next;
    *out_link = current_link(link->transport_id, link->slot);
    return true;
}

void ev_transport_set_owner(const ev_link_ref_t *owner_or_null)
{
    has_transport_owner = owner_or_null != NULL;
    if (has_transport_owner) {
        transport_owner = *owner_or_null;
    } else {
        memset(&transport_owner, 0, sizeof(transport_owner));
    }
}

esp_err_t ev_channel_set_owner_link(const ev_link_ref_t *link_or_null)
{
    pthread_mutex_lock(&route_test_lock);
    has_channel_owner = link_or_null != NULL;
    if (has_channel_owner) {
        channel_owner = *link_or_null;
    } else {
        memset(&channel_owner, 0, sizeof(channel_owner));
        if (block_fallback_route) {
            fallback_route_entered = true;
            pthread_cond_broadcast(&route_test_condition);
            while (!allow_fallback_route) {
                pthread_cond_wait(&route_test_condition, &route_test_lock);
            }
        }
    }
    pthread_mutex_unlock(&route_test_lock);
    return ESP_OK;
}

typedef struct {
    uint32_t count;
    ev_session_change_reason_t reason;
    uint32_t transition_id;
    bool had_previous;
    ev_link_ref_t previous_link;
    char previous_session_id[EV_SESSION_ID_HEX_LEN + 1];
} callback_record_t;

static callback_record_t changed;

static void session_changed(const ev_session_snapshot_t *snapshot,
                            ev_session_change_reason_t reason,
                            uint32_t transition_id,
                            const ev_link_ref_t *previous_link,
                            const char *previous_session_id)
{
    (void)snapshot;
    ++changed.count;
    changed.reason = reason;
    changed.transition_id = transition_id;
    changed.had_previous = previous_link != NULL && previous_session_id != NULL;
    if (changed.had_previous) {
        changed.previous_link = *previous_link;
        memcpy(changed.previous_session_id, previous_session_id, sizeof(changed.previous_session_id));
    }
}

static void assert_session_id(const char *session_id)
{
    assert(strlen(session_id) == EV_SESSION_ID_HEX_LEN);
    for (size_t i = 0; i < EV_SESSION_ID_HEX_LEN; ++i) {
        assert((session_id[i] >= '0' && session_id[i] <= '9') ||
               (session_id[i] >= 'a' && session_id[i] <= 'f'));
    }
}

static void test_acquire_authorize_release(void)
{
    ev_link_ref_t usj = current_link(EV_TRANSPORT_USJ, 0);
    ev_link_ref_t usb = current_link(EV_TRANSPORT_USB, 0);
    const char nonce[] = "0123456789abcdef0123456789abcdef";
    ev_session_snapshot_t session;
    assert(ev_session_acquire(&usj, nonce, 100, &session) == EV_SESSION_RESULT_OK);
    assert(session.state == EV_SESSION_OWNED && session.has_owner && !session.handshake_complete);
    assert_session_id(session.session_id);
    char session_id[sizeof(session.session_id)];
    memcpy(session_id, session.session_id, sizeof(session_id));
    assert(session.user_route == EV_TRANSPORT_USJ && session.debug_route == EV_TRANSPORT_USJ);
    assert(ev_session_authorize(&usj, session_id, false, false, 0, 101, &session) ==
           EV_SESSION_RESULT_SESSION_REQUIRED);
    assert(ev_session_hello_complete(&usj, session_id, true));
    assert(ev_session_hello_complete(&usj, session_id, true));
    assert(changed.count == 1 && changed.reason == EV_SESSION_CHANGE_ACQUIRED);
    assert(ev_session_authorize(&usj, session_id, false, false, 0, 102, &session) == EV_SESSION_RESULT_OK);
    assert(ev_session_authorize(&usj, session_id, true, false, 0, 103, &session) ==
           EV_SESSION_RESULT_VM_EPOCH_MISMATCH);
    assert(ev_session_authorize(&usj, session_id, true, true, session.vm_epoch, 104, &session) ==
           EV_SESSION_RESULT_VM_BUSY);
    ev_session_set_vm_ready(true);
    assert(ev_session_authorize(&usj, session_id, true, true, session.vm_epoch, 105, &session) ==
           EV_SESSION_RESULT_OK);

    assert(ev_session_acquire(&usb, "abcdef0123456789abcdef0123456789", 200, &session) ==
           EV_SESSION_RESULT_BUSY);
    assert(strcmp(session.session_id, session_id) == 0);
    assert(ev_session_acquire(&usj, nonce, 300, &session) == EV_SESSION_RESULT_OK);
    assert(strcmp(session.session_id, session_id) == 0);

    assert(ev_session_release_prepare(&usj, session_id, &session) == EV_SESSION_RESULT_OK);
    assert(session.state == EV_SESSION_RELEASING);
    assert(ev_session_acquire(&usb, "abcdef0123456789abcdef0123456789", 301, &session) ==
           EV_SESSION_RESULT_BUSY);
    assert(ev_session_release_finish(&usj, session_id, EV_SESSION_CHANGE_RELEASED));
    assert(ev_session_snapshot(&session) && !session.has_owner && session.state == EV_SESSION_FREE);
    assert(session.user_route == EV_TRANSPORT_USB && session.debug_route == EV_TRANSPORT_USB);
    assert(changed.reason == EV_SESSION_CHANGE_RELEASED && changed.had_previous);
    assert(link_equal(&changed.previous_link, &usj));
    assert(strcmp(changed.previous_session_id, session_id) == 0);
    assert(!ev_transport_link_is_current(&usj));
    assert(ev_transport_link_is_current(&transport_owner) == false);

    usj = current_link(EV_TRANSPORT_USJ, 0);
    assert(ev_session_acquire(&usj, nonce, 400, &session) == EV_SESSION_RESULT_OK);
    assert(strcmp(session.session_id, session_id) != 0);
    assert(ev_session_hello_complete(&usj, session.session_id, true));
    assert(ev_session_release_prepare(&usj, session.session_id, &session) == EV_SESSION_RESULT_OK);
    assert(ev_session_release_finish(&usj, session.session_id, EV_SESSION_CHANGE_RELEASED));
}

static void test_expiry_and_clock_wrap(void)
{
    ev_session_snapshot_t session;
    ev_link_ref_t usj = current_link(EV_TRANSPORT_USJ, 0);
    const char nonce[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    assert(ev_session_acquire(&usj, nonce, 1000, &session) == EV_SESSION_RESULT_OK);
    char session_id[sizeof(session.session_id)];
    memcpy(session_id, session.session_id, sizeof(session_id));
    assert(ev_session_hello_complete(&usj, session_id, true));
    assert(!ev_session_tick(1000 + EV_SESSION_LEASE_MS - 1));
    assert(ev_session_tick(1000 + EV_SESSION_LEASE_MS));
    assert(changed.reason == EV_SESSION_CHANGE_EXPIRED && changed.had_previous);
    assert(ev_session_snapshot(&session) && !session.has_owner);
    assert(session.user_route == EV_TRANSPORT_USB);

    ev_link_ref_t tcp = current_link(EV_TRANSPORT_TCP, 0);
    uint32_t start = UINT32_MAX - 500;
    assert(ev_session_acquire(&tcp, nonce, start, &session) == EV_SESSION_RESULT_OK);
    memcpy(session_id, session.session_id, sizeof(session_id));
    assert(ev_session_hello_complete(&tcp, session_id, true));
    uint32_t before_expiry = start + (EV_SESSION_LEASE_MS - 1);
    uint32_t at_expiry = start + EV_SESSION_LEASE_MS;
    assert(!ev_session_tick(before_expiry));
    assert(ev_session_tick(at_expiry));
    assert(!ev_session_snapshot(&session) || !session.has_owner);
}

typedef struct {
    ev_link_ref_t link;
    char nonce[EV_SESSION_NONCE_HEX_LEN + 1];
    ev_session_result_t result;
    ev_session_snapshot_t snapshot;
} acquire_thread_arg_t;

static void *release_thread(void *unused)
{
    (void)unused;
    ev_session_snapshot_t snapshot;
    assert(ev_session_snapshot(&snapshot) && snapshot.has_owner);
    assert(ev_session_release_prepare(&snapshot.owner_link, snapshot.session_id, &snapshot) ==
           EV_SESSION_RESULT_OK);
    assert(ev_session_release_finish(&snapshot.owner_link, snapshot.session_id,
                                     EV_SESSION_CHANGE_RELEASED));
    return NULL;
}

static void *acquire_thread(void *argument)
{
    acquire_thread_arg_t *arg = argument;
    arg->result = ev_session_acquire(&arg->link, arg->nonce, 5000, &arg->snapshot);
    return NULL;
}

static void test_route_apply_race(void)
{
    const char nonce[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    ev_link_ref_t usj = current_link(EV_TRANSPORT_USJ, 0);
    ev_session_snapshot_t session;
    assert(ev_session_acquire(&usj, nonce, 3000, &session) == EV_SESSION_RESULT_OK);
    assert(ev_session_hello_complete(&usj, session.session_id, true));

    pthread_mutex_lock(&route_test_lock);
    block_fallback_route = true;
    fallback_route_entered = false;
    allow_fallback_route = false;
    pthread_mutex_unlock(&route_test_lock);

    pthread_t releasing;
    assert(pthread_create(&releasing, NULL, release_thread, NULL) == 0);
    pthread_mutex_lock(&route_test_lock);
    while (!fallback_route_entered) {
        pthread_cond_wait(&route_test_condition, &route_test_lock);
    }
    pthread_mutex_unlock(&route_test_lock);

    acquire_thread_arg_t arg = {
        .link = current_link(EV_TRANSPORT_USB, 0),
        .nonce = "cccccccccccccccccccccccccccccccc",
    };
    pthread_t acquiring;
    assert(pthread_create(&acquiring, NULL, acquire_thread, &arg) == 0);

    bool acquisition_reserved = false;
    for (unsigned int i = 0; i < 1000; ++i) {
        assert(ev_session_snapshot(&session));
        if (session.has_owner && link_equal(&session.owner_link, &arg.link)) {
            acquisition_reserved = true;
            break;
        }
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
    }
    assert(acquisition_reserved);

    pthread_mutex_lock(&route_test_lock);
    allow_fallback_route = true;
    pthread_cond_broadcast(&route_test_condition);
    pthread_mutex_unlock(&route_test_lock);
    assert(pthread_join(releasing, NULL) == 0);
    assert(pthread_join(acquiring, NULL) == 0);
    assert(arg.result == EV_SESSION_RESULT_OK);
    assert(arg.snapshot.has_owner && link_equal(&arg.snapshot.owner_link, &arg.link));
    assert(ev_session_snapshot(&session) && session.has_owner && link_equal(&session.owner_link, &arg.link));
    assert(has_channel_owner && link_equal(&channel_owner, &arg.link));
    assert(has_transport_owner && link_equal(&transport_owner, &arg.link));

    pthread_mutex_lock(&route_test_lock);
    block_fallback_route = false;
    pthread_mutex_unlock(&route_test_lock);
    assert(ev_session_release_prepare(&arg.link, arg.snapshot.session_id, &session) == EV_SESSION_RESULT_OK);
    assert(ev_session_release_finish(&arg.link, arg.snapshot.session_id, EV_SESSION_CHANGE_RELEASED));
}

static void test_repl_queue_generation_race(void)
{
    const char old_nonce[] = "dddddddddddddddddddddddddddddddd";
    const char new_nonce[] = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    ev_link_ref_t old_link = current_link(EV_TRANSPORT_USB, 0);
    ev_session_snapshot_t old_session;
    assert(ev_session_acquire(&old_link, old_nonce, 7000, &old_session) == EV_SESSION_RESULT_OK);
    assert(ev_session_hello_complete(&old_link, old_session.session_id, true));
    (void)ev_session_vm_epoch_advance();
    ev_session_set_vm_ready(true);
    assert(ev_session_snapshot(&old_session));

    // Model a producer that passed authorization and then paused before the
    // production ring helper is reached.
    ev_session_snapshot_t authorized;
    assert(ev_session_authorize(&old_link, old_session.session_id, true, true,
                                old_session.vm_epoch, 7001, &authorized) == EV_SESSION_RESULT_OK);
    uint32_t old_generation = authorized.input_generation;
    const uint8_t old_byte = 'o';

    ev_session_snapshot_t releasing;
    assert(ev_session_release_prepare(&old_link, old_session.session_id, &releasing) ==
           EV_SESSION_RESULT_OK);
    assert(ev_session_release_finish(&old_link, old_session.session_id,
                                     EV_SESSION_CHANGE_RELEASED));
    ev_session_snapshot_t released;
    assert(ev_session_snapshot(&released) && !released.has_owner);
    uint32_t release_generation = released.input_generation;

    ev_link_ref_t new_link = current_link(EV_TRANSPORT_TCP, 0);
    ev_session_snapshot_t new_session;
    assert(ev_session_acquire(&new_link, new_nonce, 7002, &new_session) == EV_SESSION_RESULT_OK);
    assert(ev_session_hello_complete(&new_link, new_session.session_id, true));
    assert(ev_session_snapshot(&new_session));
    assert(new_session.input_generation != old_generation);

    ev_repl_queue_t queue = { 0 };
    assert(ev_repl_queue_clear_generation(&queue, new_session.input_generation,
                                          ev_session_input_generation()));
    assert(!ev_repl_queue_push(&queue, &old_byte, 1, old_generation,
                               ev_session_input_generation()));

    const uint8_t new_bytes[] = { 'n', 'e', 'w' };
    assert(ev_repl_queue_push(&queue, new_bytes, sizeof(new_bytes),
                              new_session.input_generation,
                              ev_session_input_generation()));
    // A delayed release callback cannot clear bytes already queued by the
    // new owner because its generation no longer matches the live session.
    assert(!ev_repl_queue_clear_generation(&queue, release_generation,
                                           ev_session_input_generation()));
    assert(ev_repl_queue_pending(&queue, new_session.input_generation,
                                 ev_session_input_generation(), true));
    for (size_t i = 0; i < sizeof(new_bytes); ++i) {
        uint8_t value = 0;
        assert(ev_repl_queue_pop(&queue, new_session.input_generation,
                                 ev_session_input_generation(), true, &value));
        assert(value == new_bytes[i]);
    }
    uint8_t value = 0;
    assert(!ev_repl_queue_pop(&queue, new_session.input_generation,
                              ev_session_input_generation(), true, &value));

    assert(ev_session_release_prepare(&new_link, new_session.session_id, &new_session) ==
           EV_SESSION_RESULT_OK);
    assert(ev_session_release_finish(&new_link, new_session.session_id,
                                     EV_SESSION_CHANGE_RELEASED));
}

int main(void)
{
    for (ev_transport_id_t id = EV_TRANSPORT_USJ; id < EV_TRANSPORT_MAX; ++id) {
        for (uint8_t slot = 0; slot < EV_TRANSPORT_LINK_SLOTS_MAX; ++slot) {
            opened[id][slot] = true;
            generations[id][slot] = 1;
        }
    }
    assert(ev_session_init0() == ESP_OK);
    assert(ev_session_init0() == ESP_OK);
    assert(ev_session_vm_epoch() != 0);
    assert(ev_session_snapshot(&(ev_session_snapshot_t) {
        0
    }));
    ev_session_set_changed_cb(session_changed);
    test_acquire_authorize_release();
    test_expiry_and_clock_wrap();
    test_route_apply_race();
    test_repl_queue_generation_race();
    puts("ev_session host tests passed");
    return 0;
}

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ev_session.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "boardconfig.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ev_channel.h"

#ifndef ESP_VISION_TRANSPORT_DEFAULT_SINK
#define ESP_VISION_TRANSPORT_DEFAULT_SINK "usj"
#endif

typedef struct {
    ev_session_state_t state;
    ev_link_ref_t owner_link;
    char session_id[EV_SESSION_ID_HEX_LEN + 1];
    char client_nonce[EV_SESSION_NONCE_HEX_LEN + 1];
    uint32_t last_activity_ms;
    uint32_t vm_epoch;
    uint32_t input_generation;
    uint32_t transition_id;
    bool handshake_complete;
    bool vm_ready;
    ev_transport_id_t no_owner_route;
} ev_session_state_data_t;

static ev_session_state_data_t s_session;
static ev_session_changed_cb_t s_changed_cb;
static StaticSemaphore_t s_session_lock_buf;
static SemaphoreHandle_t s_session_lock;
static StaticSemaphore_t s_route_apply_lock_buf;
static SemaphoreHandle_t s_route_apply_lock;
static bool s_initialized;
static volatile uint32_t s_input_generation_atomic;

static bool ev_session_clear_owner(const ev_link_ref_t *link,
                                   ev_session_change_reason_t reason,
                                   bool require_releasing,
                                   const char *session_id);

static void ev_session_lock(void)
{
    if (s_session_lock == NULL) {
        s_session_lock = xSemaphoreCreateMutexStatic(&s_session_lock_buf);
    }
    if (s_session_lock != NULL) {
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
    }
}

static void ev_session_unlock(void)
{
    if (s_session_lock != NULL) {
        xSemaphoreGive(s_session_lock);
    }
}

static void ev_session_route_apply_lock(void)
{
    if (s_route_apply_lock == NULL) {
        s_route_apply_lock = xSemaphoreCreateMutexStatic(&s_route_apply_lock_buf);
    }
    if (s_route_apply_lock != NULL) {
        xSemaphoreTake(s_route_apply_lock, portMAX_DELAY);
    }
}

static void ev_session_route_apply_unlock(void)
{
    if (s_route_apply_lock != NULL) {
        xSemaphoreGive(s_route_apply_lock);
    }
}

static uint32_t ev_session_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool ev_session_link_equal(const ev_link_ref_t *a, const ev_link_ref_t *b)
{
    return (a != NULL) && (b != NULL) && (a->transport_id == b->transport_id) &&
           (a->slot == b->slot) && (a->generation == b->generation);
}

static bool ev_session_hex_nonce_valid(const char *nonce)
{
    if ((nonce == NULL) || (strlen(nonce) != EV_SESSION_NONCE_HEX_LEN)) {
        return false;
    }
    for (size_t i = 0; i < EV_SESSION_NONCE_HEX_LEN; ++i) {
        char c = nonce[i];
        if (!(((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')))) {
            return false;
        }
    }
    return true;
}

static uint32_t ev_session_next_counter(uint32_t counter)
{
    ++counter;
    return counter == 0 ? 1 : counter;
}

static void ev_session_bump_input_generation_locked(void)
{
    s_session.input_generation = ev_session_next_counter(s_session.input_generation);
    __atomic_store_n(&s_input_generation_atomic, s_session.input_generation, __ATOMIC_RELEASE);
}

static void ev_session_make_id(char out[EV_SESSION_ID_HEX_LEN + 1])
{
    int len = snprintf(out, EV_SESSION_ID_HEX_LEN + 1,
                       "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32,
                       esp_random(), esp_random(), esp_random(), esp_random());
    if ((len != EV_SESSION_ID_HEX_LEN) || (out[0] == '\0')) {
        // esp_random is a hardware-backed primitive in production; this path
        // is only a defensive fallback for unexpected formatting failure.
        memset(out, '0', EV_SESSION_ID_HEX_LEN);
        out[EV_SESSION_ID_HEX_LEN] = '\0';
        out[EV_SESSION_ID_HEX_LEN - 1] = '1';
    }
}

static void ev_session_snapshot_locked(uint32_t now_ms, ev_session_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s_session.state;
    out->has_owner = (s_session.state != EV_SESSION_FREE);
    out->vm_epoch = s_session.vm_epoch;
    out->input_generation = s_session.input_generation;
    out->transition_id = s_session.transition_id;
    out->handshake_complete = s_session.handshake_complete;
    out->vm_ready = s_session.vm_ready;
    out->user_route = s_session.no_owner_route;
    out->debug_route = out->user_route;
    if (out->has_owner) {
        out->owner_link = s_session.owner_link;
        out->user_route = s_session.owner_link.transport_id;
        out->debug_route = s_session.owner_link.transport_id;
        out->user_route_slot = s_session.owner_link.slot;
        out->debug_route_slot = s_session.owner_link.slot;
        memcpy(out->session_id, s_session.session_id, sizeof(out->session_id));
        memcpy(out->client_nonce, s_session.client_nonce, sizeof(out->client_nonce));
        uint32_t elapsed = (uint32_t)(now_ms - s_session.last_activity_ms);
        out->lease_remaining_ms = (elapsed >= EV_SESSION_LEASE_MS) ? 0 : EV_SESSION_LEASE_MS - elapsed;
    }
}

static bool ev_session_apply_route(const ev_session_snapshot_t *snapshot)
{
    bool is_current;
    ev_session_route_apply_lock();
    ev_session_lock();
    is_current = s_session.transition_id == snapshot->transition_id &&
                 (s_session.state == snapshot->state) &&
                 (!snapshot->has_owner || ev_session_link_equal(&s_session.owner_link, &snapshot->owner_link));
    ev_session_unlock();
    if (is_current) {
        if (snapshot->has_owner) {
            ev_transport_set_owner(&snapshot->owner_link);
            (void)ev_channel_set_owner_link(&snapshot->owner_link);
        } else {
            ev_transport_set_owner(NULL);
            (void)ev_channel_set_owner_link(NULL);
        }
    }
    ev_session_route_apply_unlock();
    return is_current;
}

static void ev_session_notify_change(const ev_session_snapshot_t *snapshot,
                                     ev_session_change_reason_t reason,
                                     uint32_t transition_id,
                                     ev_session_changed_cb_t callback,
                                     const ev_link_ref_t *previous_link,
                                     const char *previous_session_id)
{
    if (callback != NULL) {
        callback(snapshot, reason, transition_id, previous_link, previous_session_id);
    }
}

esp_err_t ev_session_init0(void)
{
    ev_session_lock();
    if (s_initialized) {
        ev_session_unlock();
        return ESP_OK;
    }
    memset(&s_session, 0, sizeof(s_session));
    s_session.state = EV_SESSION_FREE;
    /* A hard reboot starts a fresh protocol VM generation. Soft resets keep
     * advancing this value in ev_control_transport_vm_reset_begin(). */
    s_session.vm_epoch = esp_random();
    if (s_session.vm_epoch == 0) {
        s_session.vm_epoch = 1;
    }
    s_session.input_generation = 1;
    __atomic_store_n(&s_input_generation_atomic, s_session.input_generation, __ATOMIC_RELEASE);
    s_session.vm_ready = false;
    (void)ev_transport_id_from_name(ESP_VISION_TRANSPORT_DEFAULT_SINK, &s_session.no_owner_route);
    s_session.transition_id = 0;
    s_initialized = true;
    ev_session_unlock();
    ev_transport_set_owner(NULL);
    return ESP_OK;
}

void ev_session_set_changed_cb(ev_session_changed_cb_t callback)
{
    ev_session_lock();
    s_changed_cb = callback;
    ev_session_unlock();
}

bool ev_session_snapshot(ev_session_snapshot_t *out_snapshot)
{
    if ((out_snapshot == NULL) || !s_initialized) {
        return false;
    }
    ev_session_lock();
    ev_session_snapshot_locked(ev_session_now_ms(), out_snapshot);
    ev_session_unlock();
    return true;
}

ev_session_result_t ev_session_acquire(const ev_link_ref_t *link,
                                       const char *client_nonce,
                                       uint32_t now_ms,
                                       ev_session_snapshot_t *out_snapshot)
{
    if ((link == NULL) || (out_snapshot == NULL) || !ev_session_hex_nonce_valid(client_nonce)) {
        return EV_SESSION_RESULT_INVALID_ARGUMENT;
    }
    if (!ev_transport_link_is_current(link)) {
        return EV_SESSION_RESULT_STALE_LINK;
    }

    bool acquired = false;
    ev_session_lock();
    if (!s_initialized) {
        ev_session_unlock();
        return EV_SESSION_RESULT_INVALID_ARGUMENT;
    }
    if (s_session.state == EV_SESSION_FREE) {
        s_session.state = EV_SESSION_OWNED;
        s_session.owner_link = *link;
        memcpy(s_session.client_nonce, client_nonce, EV_SESSION_NONCE_HEX_LEN + 1);
        ev_session_make_id(s_session.session_id);
        s_session.last_activity_ms = now_ms;
        s_session.handshake_complete = false;
        s_session.transition_id = ev_session_next_counter(s_session.transition_id);
        ev_session_bump_input_generation_locked();
        acquired = true;
    } else if ((s_session.state == EV_SESSION_OWNED) &&
               ev_session_link_equal(link, &s_session.owner_link) &&
               (strcmp(client_nonce, s_session.client_nonce) == 0)) {
        // A repeated hello for the same logical connection is idempotent.
        s_session.last_activity_ms = now_ms;
    } else {
        ev_session_snapshot_locked(now_ms, out_snapshot);
        ev_session_unlock();
        return EV_SESSION_RESULT_BUSY;
    }
    ev_session_snapshot_locked(now_ms, out_snapshot);
    ev_session_unlock();

    if (!ev_transport_link_is_current(link)) {
        (void)ev_session_link_lost(link, EV_SESSION_CHANGE_LINK_LOST);
        return EV_SESSION_RESULT_STALE_LINK;
    }
    if (acquired) {
        (void)ev_session_apply_route(out_snapshot);
    }
    return EV_SESSION_RESULT_OK;
}

static ev_session_result_t ev_session_check_owner_locked(const ev_link_ref_t *link,
                                                         const char *session_id,
                                                         uint32_t now_ms,
                                                         ev_session_snapshot_t *out_snapshot)
{
    if ((s_session.state == EV_SESSION_FREE) || (s_session.state == EV_SESSION_RELEASING)) {
        return EV_SESSION_RESULT_SESSION_REQUIRED;
    }
    if (!ev_session_link_equal(link, &s_session.owner_link) ||
            (session_id == NULL) || (strcmp(session_id, s_session.session_id) != 0)) {
        return EV_SESSION_RESULT_NOT_OWNER;
    }
    if (!s_session.handshake_complete) {
        return EV_SESSION_RESULT_SESSION_REQUIRED;
    }
    ev_session_snapshot_locked(now_ms, out_snapshot);
    return EV_SESSION_RESULT_OK;
}

ev_session_result_t ev_session_keepalive(const ev_link_ref_t *link,
                                         const char *session_id,
                                         uint32_t now_ms,
                                         ev_session_snapshot_t *out_snapshot)
{
    if ((link == NULL) || (session_id == NULL) || (out_snapshot == NULL)) {
        return EV_SESSION_RESULT_SESSION_REQUIRED;
    }
    if (!ev_transport_link_is_current(link)) {
        return EV_SESSION_RESULT_STALE_LINK;
    }
    ev_session_lock();
    ev_session_result_t result = ev_session_check_owner_locked(link, session_id, now_ms, out_snapshot);
    if (result == EV_SESSION_RESULT_OK) {
        s_session.last_activity_ms = now_ms;
        ev_session_snapshot_locked(now_ms, out_snapshot);
    }
    ev_session_unlock();
    return result;
}

ev_session_result_t ev_session_authorize(const ev_link_ref_t *link,
                                         const char *session_id,
                                         bool vm_related,
                                         bool has_vm_epoch,
                                         uint32_t vm_epoch,
                                         uint32_t now_ms,
                                         ev_session_snapshot_t *out_snapshot)
{
    if ((link == NULL) || (out_snapshot == NULL)) {
        return EV_SESSION_RESULT_SESSION_REQUIRED;
    }
    if (!ev_transport_link_is_current(link)) {
        return EV_SESSION_RESULT_STALE_LINK;
    }
    ev_session_lock();
    ev_session_result_t result = ev_session_check_owner_locked(link, session_id, now_ms, out_snapshot);
    if ((result == EV_SESSION_RESULT_OK) && vm_related) {
        if (!has_vm_epoch) {
            result = EV_SESSION_RESULT_VM_EPOCH_MISMATCH;
        } else if (vm_epoch != s_session.vm_epoch) {
            result = EV_SESSION_RESULT_VM_EPOCH_MISMATCH;
        } else if (!s_session.vm_ready) {
            result = EV_SESSION_RESULT_VM_BUSY;
        }
    }
    if ((result == EV_SESSION_RESULT_OK) && !s_session.handshake_complete) {
        result = EV_SESSION_RESULT_SESSION_REQUIRED;
    }
    if (result == EV_SESSION_RESULT_OK) {
        // Invalid VM epochs and requests while the interpreter is restarting
        // must not keep an otherwise idle session alive.
        s_session.last_activity_ms = now_ms;
        ev_session_snapshot_locked(now_ms, out_snapshot);
    }
    if (result != EV_SESSION_RESULT_OK) {
        ev_session_snapshot_locked(now_ms, out_snapshot);
    }
    ev_session_unlock();
    return result;
}

ev_session_result_t ev_session_release_prepare(const ev_link_ref_t *link,
                                               const char *session_id,
                                               ev_session_snapshot_t *out_snapshot)
{
    if ((link == NULL) || (session_id == NULL) || (out_snapshot == NULL)) {
        return EV_SESSION_RESULT_SESSION_REQUIRED;
    }
    if (!ev_transport_link_is_current(link)) {
        return EV_SESSION_RESULT_STALE_LINK;
    }
    ev_session_lock();
    if ((s_session.state == EV_SESSION_FREE) || (s_session.state == EV_SESSION_RELEASING)) {
        ev_session_unlock();
        return EV_SESSION_RESULT_SESSION_REQUIRED;
    }
    if (!ev_session_link_equal(link, &s_session.owner_link) ||
            (strcmp(session_id, s_session.session_id) != 0)) {
        ev_session_unlock();
        return EV_SESSION_RESULT_NOT_OWNER;
    }
    s_session.state = EV_SESSION_RELEASING;
    ev_session_bump_input_generation_locked();
    ev_session_snapshot_locked(ev_session_now_ms(), out_snapshot);
    ev_session_unlock();
    return EV_SESSION_RESULT_OK;
}

bool ev_session_hello_complete(const ev_link_ref_t *link,
                               const char *session_id,
                               bool tx_succeeded)
{
    if ((link == NULL) || (session_id == NULL)) {
        return false;
    }
    if (!tx_succeeded) {
        return ev_session_clear_owner(link, EV_SESSION_CHANGE_TX_FAILED, false, session_id);
    }
    ev_session_snapshot_t snapshot;
    ev_session_changed_cb_t callback;
    ev_session_lock();
    if ((s_session.state != EV_SESSION_OWNED) || !ev_session_link_equal(link, &s_session.owner_link) ||
            (strcmp(session_id, s_session.session_id) != 0)) {
        ev_session_unlock();
        return false;
    }
    if (s_session.handshake_complete) {
        ev_session_unlock();
        return true;
    }
    s_session.handshake_complete = true;
    ev_session_snapshot_locked(ev_session_now_ms(), &snapshot);
    callback = s_changed_cb;
    ev_session_unlock();
    if (ev_session_apply_route(&snapshot)) {
        ev_session_notify_change(&snapshot, EV_SESSION_CHANGE_ACQUIRED,
                                 snapshot.transition_id, callback, NULL, NULL);
    }
    return true;
}

bool ev_session_output_ready(const ev_link_ref_t *link, const char *session_id)
{
    if ((link == NULL) || (session_id == NULL) || !ev_transport_link_is_current(link)) {
        return false;
    }
    ev_session_lock();
    bool ready = (s_session.state == EV_SESSION_OWNED) && s_session.handshake_complete &&
                 ev_session_link_equal(link, &s_session.owner_link) &&
                 (strcmp(session_id, s_session.session_id) == 0);
    ev_session_unlock();
    return ready;
}

static bool ev_session_clear_owner(const ev_link_ref_t *link,
                                   ev_session_change_reason_t reason,
                                   bool require_releasing,
                                   const char *session_id)
{
    if (link == NULL) {
        return false;
    }
    ev_session_snapshot_t snapshot;
    ev_session_changed_cb_t callback;
    uint32_t transition_id;
    char previous_session_id[EV_SESSION_ID_HEX_LEN + 1];
    ev_link_ref_t previous_link;
    ev_session_lock();
    if ((s_session.state == EV_SESSION_FREE) || !ev_session_link_equal(link, &s_session.owner_link) ||
            (require_releasing && (s_session.state != EV_SESSION_RELEASING)) ||
            ((session_id != NULL) && (strcmp(session_id, s_session.session_id) != 0))) {
        ev_session_unlock();
        return false;
    }
    memcpy(previous_session_id, s_session.session_id, sizeof(previous_session_id));
    previous_link = s_session.owner_link;
    // Rotate serial generations before making the session FREE. A competing
    // hello that cached the old ref cannot claim the just-released byte stream.
    if ((link->transport_id == EV_TRANSPORT_USJ) || (link->transport_id == EV_TRANSPORT_UART)) {
        ev_link_ref_t advanced;
        (void)ev_transport_link_advance_generation(link, &advanced);
    }
    memset(s_session.session_id, 0, sizeof(s_session.session_id));
    memset(s_session.client_nonce, 0, sizeof(s_session.client_nonce));
    memset(&s_session.owner_link, 0, sizeof(s_session.owner_link));
    s_session.state = EV_SESSION_FREE;
    s_session.handshake_complete = false;
    ev_session_bump_input_generation_locked();
    if (!ev_transport_id_from_name(ESP_VISION_TRANSPORT_FALLBACK_SINK, &s_session.no_owner_route)) {
        (void)ev_transport_id_from_name(ESP_VISION_TRANSPORT_DEFAULT_SINK, &s_session.no_owner_route);
    }
    s_session.transition_id = ev_session_next_counter(s_session.transition_id);
    transition_id = s_session.transition_id;
    callback = s_changed_cb;
    ev_session_snapshot_locked(ev_session_now_ms(), &snapshot);
    ev_session_unlock();

    if (ev_session_apply_route(&snapshot)) {
        ev_session_notify_change(&snapshot, reason, transition_id, callback,
                                 &previous_link, previous_session_id);
    }
    return true;
}

bool ev_session_release_finish(const ev_link_ref_t *link,
                               const char *session_id,
                               ev_session_change_reason_t reason)
{
    return ev_session_clear_owner(link, reason, true, session_id);
}

bool ev_session_link_lost(const ev_link_ref_t *link,
                          ev_session_change_reason_t reason)
{
    return ev_session_clear_owner(link, reason, false, NULL);
}

bool ev_session_tick(uint32_t now_ms)
{
    ev_link_ref_t expired_link;
    ev_session_snapshot_t snapshot;
    ev_session_changed_cb_t callback;
    uint32_t transition_id;
    char previous_session_id[EV_SESSION_ID_HEX_LEN + 1];
    ev_session_lock();
    if ((s_session.state != EV_SESSION_OWNED) ||
            ((uint32_t)(now_ms - s_session.last_activity_ms) < EV_SESSION_LEASE_MS)) {
        ev_session_unlock();
        return false;
    }
    expired_link = s_session.owner_link;
    memcpy(previous_session_id, s_session.session_id, sizeof(previous_session_id));
    if ((expired_link.transport_id == EV_TRANSPORT_USJ) ||
            (expired_link.transport_id == EV_TRANSPORT_UART)) {
        ev_link_ref_t advanced;
        (void)ev_transport_link_advance_generation(&expired_link, &advanced);
    }
    memset(s_session.session_id, 0, sizeof(s_session.session_id));
    memset(s_session.client_nonce, 0, sizeof(s_session.client_nonce));
    memset(&s_session.owner_link, 0, sizeof(s_session.owner_link));
    s_session.state = EV_SESSION_FREE;
    s_session.handshake_complete = false;
    ev_session_bump_input_generation_locked();
    if (!ev_transport_id_from_name(ESP_VISION_TRANSPORT_FALLBACK_SINK, &s_session.no_owner_route)) {
        (void)ev_transport_id_from_name(ESP_VISION_TRANSPORT_DEFAULT_SINK, &s_session.no_owner_route);
    }
    s_session.transition_id = ev_session_next_counter(s_session.transition_id);
    transition_id = s_session.transition_id;
    callback = s_changed_cb;
    ev_session_snapshot_locked(now_ms, &snapshot);
    ev_session_unlock();

    if (ev_session_apply_route(&snapshot)) {
        ev_session_notify_change(&snapshot, EV_SESSION_CHANGE_EXPIRED, transition_id,
                                 callback, &expired_link, previous_session_id);
    }
    return true;
}

uint32_t ev_session_vm_epoch(void)
{
    ev_session_lock();
    uint32_t epoch = s_session.vm_epoch;
    ev_session_unlock();
    return epoch;
}

uint32_t ev_session_vm_epoch_advance(void)
{
    ev_session_lock();
    s_session.vm_epoch = ev_session_next_counter(s_session.vm_epoch);
    ev_session_bump_input_generation_locked();
    uint32_t epoch = s_session.vm_epoch;
    ev_session_unlock();
    return epoch;
}

uint32_t ev_session_input_generation(void)
{
    return __atomic_load_n(&s_input_generation_atomic, __ATOMIC_ACQUIRE);
}

void ev_session_set_vm_ready(bool ready)
{
    ev_session_lock();
    s_session.vm_ready = ready;
    ev_session_unlock();
}

bool ev_session_vm_ready(void)
{
    ev_session_lock();
    bool ready = s_session.vm_ready;
    ev_session_unlock();
    return ready;
}

const char *ev_session_result_code(ev_session_result_t result)
{
    switch (result) {
    case EV_SESSION_RESULT_OK: return "OK";
    case EV_SESSION_RESULT_BUSY: return "SESSION_BUSY";
    case EV_SESSION_RESULT_SESSION_REQUIRED: return "SESSION_REQUIRED";
    case EV_SESSION_RESULT_NOT_OWNER: return "NOT_OWNER";
    case EV_SESSION_RESULT_VM_BUSY: return "VM_BUSY";
    case EV_SESSION_RESULT_VM_EPOCH_MISMATCH: return "VM_EPOCH_MISMATCH";
    case EV_SESSION_RESULT_STALE_LINK: return "NOT_OWNER";
    case EV_SESSION_RESULT_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    default: return "INTERNAL_ERROR";
    }
}

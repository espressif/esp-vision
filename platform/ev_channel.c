/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ev_channel.h"

#include <string.h>

#include "boardconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "py/mphal.h"

#ifndef ESP_VISION_TRANSPORT_DEFAULT_SINK
#define ESP_VISION_TRANSPORT_DEFAULT_SINK "usj"
#endif
#ifndef ESP_VISION_TRANSPORT_FALLBACK_SINK
#define ESP_VISION_TRANSPORT_FALLBACK_SINK ESP_VISION_TRANSPORT_DEFAULT_SINK
#endif

typedef struct {
    const char *id;
    bool ready;
    ssize_t (*write)(const void *buf, size_t len);
} ev_local_sink_t;

static ev_link_ref_t s_owner_link;
static bool s_has_owner;
static const char *s_no_owner_sink = ESP_VISION_TRANSPORT_DEFAULT_SINK;
static bool s_initialized;
static ev_channel_route_changed_cb_t s_route_changed_cb;
static StaticSemaphore_t s_route_lock_buf;
static SemaphoreHandle_t s_route_lock;

static ssize_t ev_console_write(const void *buf, size_t len)
{
    return (ssize_t)mp_hal_stdout_tx_strn((const char *)buf, (mp_uint_t)len);
}

static ssize_t ev_null_write(const void *buf, size_t len)
{
    (void)buf;
    return (ssize_t)len;
}

static const ev_local_sink_t s_local_sinks[] = {
    { "console", true, ev_console_write },
    { "null", true, ev_null_write },
};

static void ev_route_lock(void)
{
    if (s_route_lock == NULL) {
        s_route_lock = xSemaphoreCreateMutexStatic(&s_route_lock_buf);
    }
    if (s_route_lock != NULL) {
        xSemaphoreTake(s_route_lock, portMAX_DELAY);
    }
}

static void ev_route_unlock(void)
{
    if (s_route_lock != NULL) {
        xSemaphoreGive(s_route_lock);
    }
}

static void ev_channel_ensure_init(void)
{
    if (!__atomic_load_n(&s_initialized, __ATOMIC_ACQUIRE)) {
        ev_channel_init0();
    }
}

static const ev_local_sink_t *ev_local_sink_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_local_sinks) / sizeof(s_local_sinks[0]); ++i) {
        if (strcmp(id, s_local_sinks[i].id) == 0) {
            return &s_local_sinks[i];
        }
    }
    if (strcmp(id, "none") == 0) {
        return &s_local_sinks[1];
    }
    return NULL;
}

static bool ev_channel_no_owner_link(const char *sink_id, ev_link_ref_t *out_link)
{
    ev_transport_id_t id;
    return ev_transport_id_from_name(sink_id, &id) &&
           ev_transport_link_ref(id, EV_TRANSPORT_PHYSICAL_SLOT, out_link);
}

void ev_channel_init0(void)
{
    ev_route_lock();
    if (!__atomic_load_n(&s_initialized, __ATOMIC_RELAXED)) {
        // Channel ownership is platform/session state and survives VM resets.
        // Repeated initialization must not detach a live session from its route.
        memset(&s_owner_link, 0, sizeof(s_owner_link));
        s_has_owner = false;
        s_no_owner_sink = ESP_VISION_TRANSPORT_DEFAULT_SINK;
        __atomic_store_n(&s_initialized, true, __ATOMIC_RELEASE);
    }
    ev_route_unlock();
}

void ev_channel_poll_auto(void)
{
    // v4 has no DTR-triggered route stealing. Retained as a no-op for older
    // internal callers while all route changes move through ev_session.
}

void ev_channel_set_route_changed_cb(ev_channel_route_changed_cb_t callback)
{
    ev_route_lock();
    s_route_changed_cb = callback;
    ev_route_unlock();
}

esp_err_t ev_channel_set_owner_link(const ev_link_ref_t *link_or_null)
{
    ev_channel_ensure_init();
    if ((link_or_null != NULL) && !ev_transport_link_is_current(link_or_null)) {
        return ESP_ERR_INVALID_STATE;
    }
    ev_route_lock();
    s_has_owner = link_or_null != NULL;
    if (s_has_owner) {
        s_owner_link = *link_or_null;
    } else {
        memset(&s_owner_link, 0, sizeof(s_owner_link));
        s_no_owner_sink = ESP_VISION_TRANSPORT_FALLBACK_SINK;
    }
    ev_link_ref_t target;
    bool has_target = s_has_owner ? (target = s_owner_link, true) :
                      ev_channel_no_owner_link(s_no_owner_sink, &target);
    const char *target_name = has_target ? ev_transport_name(target.transport_id) :
                              (s_has_owner ? "null" : s_no_owner_sink);
    ev_channel_route_changed_cb_t callback = s_route_changed_cb;
    bool has_owner = s_has_owner;
    ev_route_unlock();

    // Legacy callback is retained for internal consumers. v4 session events
    // are emitted by ev_session and carry the shared transition identifier.
    if (callback != NULL) {
        callback(EV_STREAM_USER, target_name, false, has_owner ? "session_acquired" : "session_released");
        callback(EV_STREAM_DEBUG, target_name, false, has_owner ? "session_acquired" : "session_released");
    }
    return ESP_OK;
}

bool ev_channel_target_link(ev_stream_t stream, ev_link_ref_t *out_link)
{
    if ((stream >= EV_STREAM_MAX) || (out_link == NULL)) {
        return false;
    }
    ev_channel_ensure_init();
    ev_route_lock();
    bool has_owner = s_has_owner;
    ev_link_ref_t owner = s_owner_link;
    const char *no_owner_sink = s_no_owner_sink;
    ev_route_unlock();
    if (has_owner) {
        if (ev_transport_link_is_current(&owner)) {
            *out_link = owner;
            return true;
        }
        return false;
    }
    return ev_channel_no_owner_link(no_owner_sink, out_link);
}

static bool ev_channel_sink_link(const char *sink_id, uint8_t slot, ev_link_ref_t *out_link)
{
    ev_transport_id_t id;
    return ev_transport_id_from_name(sink_id, &id) && ev_transport_link_ref(id, slot, out_link);
}

esp_err_t ev_channel_bind(ev_stream_t stream, const char *sink_id)
{
    (void)stream;
    (void)sink_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ev_channel_bind_auto(ev_stream_t stream)
{
    (void)stream;
    return ESP_ERR_NOT_SUPPORTED;
}

const char *ev_channel_get(ev_stream_t stream)
{
    if (stream >= EV_STREAM_MAX) {
        return "null";
    }
    ev_channel_ensure_init();
    ev_route_lock();
    const char *name = s_has_owner ? ev_transport_name(s_owner_link.transport_id) : s_no_owner_sink;
    ev_route_unlock();
    return name;
}

bool ev_channel_is_auto(ev_stream_t stream)
{
    (void)stream;
    return false;
}

bool ev_channel_ready(ev_stream_t stream)
{
    ev_link_ref_t target;
    if (!ev_channel_target_link(stream, &target)) {
        return false;
    }
    ev_transport_endpoint_state_t state;
    ev_transport_endpoint_state(target.transport_id, target.slot, &state);
    return state.ready;
}

bool ev_channel_sink_supported(const char *sink_id)
{
    ev_transport_id_t id;
    if (!ev_transport_id_from_name(sink_id, &id)) {
        return ev_local_sink_find(sink_id) != NULL;
    }
    ev_transport_endpoint_state_t state;
    ev_transport_endpoint_state(id, EV_TRANSPORT_PHYSICAL_SLOT, &state);
    return state.supported;
}

bool ev_channel_sink_ready(const char *sink_id)
{
    ev_transport_id_t id;
    if (ev_transport_id_from_name(sink_id, &id)) {
        ev_transport_endpoint_state_t state;
        ev_transport_endpoint_state(id, EV_TRANSPORT_PHYSICAL_SLOT, &state);
        return state.ready;
    }
    const ev_local_sink_t *sink = ev_local_sink_find(sink_id);
    return sink != NULL && sink->ready;
}

bool ev_channel_sink_present(const char *sink_id)
{
    ev_transport_id_t id;
    if (!ev_transport_id_from_name(sink_id, &id)) {
        return ev_local_sink_find(sink_id) != NULL;
    }
    ev_transport_endpoint_state_t state;
    ev_transport_endpoint_state(id, EV_TRANSPORT_PHYSICAL_SLOT, &state);
    return state.present_known && state.present;
}

const char *ev_channel_sink_state(const char *sink_id)
{
    ev_transport_id_t id;
    if (!ev_transport_id_from_name(sink_id, &id)) {
        return ev_local_sink_find(sink_id) != NULL ? "active" : "unknown";
    }
    ev_transport_endpoint_state_t state;
    ev_transport_endpoint_state(id, EV_TRANSPORT_PHYSICAL_SLOT, &state);
    if (!state.supported) {
        return "unsupported";
    }
    if (state.ready) {
        return "active";
    }
    if (state.present_known && state.present) {
        return "present";
    }
    return "idle";
}

ssize_t ev_channel_write(ev_stream_t stream, const void *buf, size_t len)
{
    if ((stream >= EV_STREAM_MAX) || (buf == NULL) || (len == 0)) {
        return 0;
    }
    ev_link_ref_t target;
    if (!ev_channel_target_link(stream, &target)) {
        return -1;
    }
    ev_io_result_t result = ev_transport_write(&target, buf, len);
    if (result.status == EV_IO_PROGRESS) {
        return (ssize_t)result.transferred;
    }
    return result.status == EV_IO_WOULD_BLOCK ? 0 : -1;
}

ssize_t ev_channel_write_sink_if_present(const char *sink_id, const void *buf, size_t len)
{
    if ((sink_id == NULL) || (buf == NULL) || (len == 0)) {
        return 0;
    }
    ev_link_ref_t link;
    if (ev_channel_sink_link(sink_id, EV_TRANSPORT_PHYSICAL_SLOT, &link)) {
        ev_io_result_t result = ev_transport_write(&link, buf, len);
        if (result.status == EV_IO_PROGRESS) {
            return (ssize_t)result.transferred;
        }
        return result.status == EV_IO_WOULD_BLOCK ? 0 : -1;
    }
    const ev_local_sink_t *sink = ev_local_sink_find(sink_id);
    return (sink != NULL && sink->ready) ? sink->write(buf, len) : -1;
}

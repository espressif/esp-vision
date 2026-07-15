/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ev_channel.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "py/mpconfig.h"
#include "py/mphal.h"

#if MICROPY_HW_ESP_USB_SERIAL_JTAG
#include "usb_serial_jtag.h"
#endif

#if MICROPY_HW_ENABLE_UART_REPL
#include "uart.h"
#endif

#if MICROPY_HW_USB_CDC
#include "shared/tinyusb/mp_usbd_cdc.h"
#include "tusb.h"

// Implemented in the overlay shared/tinyusb/mp_usbd_cdc.c. Unlike
// mp_usbd_cdc_tx_strn it never touches MicroPython scheduler/thread state,
// so frame writes are safe from any task (VM task, transport task, log hooks).
extern mp_uint_t mp_usbd_cdc_tx_strn_anytask(const char *str, mp_uint_t len);
#endif

#include "boardconfig.h"

// Both streams prefer USJ and auto-follow CDC DTR when USB-OTG is available.
// A board may override this when its physical USB topology requires a fixed
// route.
#ifndef ESP_VISION_AUTO_CDC_ROUTE
#define ESP_VISION_AUTO_CDC_ROUTE (MICROPY_HW_USB_CDC)
#endif

typedef struct {
    const char *id;
    bool (*ready)(void);
    ssize_t (*write)(const void *buf, size_t len);
} ev_sink_t;

typedef struct {
    const ev_sink_t *sink;
    bool auto_route;
} ev_route_t;

static ev_route_t s_routes[EV_STREAM_MAX];
static bool s_initialized;
static ev_channel_route_changed_cb_t s_route_changed_cb;

// Route state is accessed from the VM task (frame writes, boot init), the
// EV transport task (poll_auto, RPC dispatch), and any task emitting hooked
// ESP_LOG output. All accessors take this recursive mutex.
// Lock ordering: the route lock may be held while taking a per-sink frame
// mutex (ev_mux.c), never the other way around.
static StaticSemaphore_t s_route_lock_buf;
static SemaphoreHandle_t s_route_lock;

static void ev_route_lock(void)
{
    if (s_route_lock == NULL) {
        s_route_lock = xSemaphoreCreateRecursiveMutexStatic(&s_route_lock_buf);
    }
    xSemaphoreTakeRecursive(s_route_lock, portMAX_DELAY);
}

static void ev_route_unlock(void)
{
    xSemaphoreGiveRecursive(s_route_lock);
}

static bool ev_sink_usj_ready(void)
{
#if MICROPY_HW_ESP_USB_SERIAL_JTAG
    return true;
#else
    return false;
#endif
}

static ssize_t ev_sink_usj_write(const void *buf, size_t len)
{
#if MICROPY_HW_ESP_USB_SERIAL_JTAG
    return (ssize_t)usb_serial_jtag_tx_strn_count(buf, len);
#else
    (void)buf;
    (void)len;
    return -1;
#endif
}

static bool ev_sink_cdc_ready(void)
{
#if MICROPY_HW_USB_CDC
    return tusb_inited() && tud_cdc_connected();
#else
    return false;
#endif
}

static bool ev_sink_cdc_present(void)
{
#if MICROPY_HW_USB_CDC
    // "present" = the OTG interface is enumerated to a host (cable plugged);
    // "ready" additionally requires the host to hold the port open (DTR).
    // Routing only ever uses ready; present is informational (capabilities,
    // transport.state, and the IDE's "OTG available" hint).
    return tusb_inited() && tud_connected();
#else
    return false;
#endif
}

static ssize_t ev_sink_cdc_write(const void *buf, size_t len)
{
#if MICROPY_HW_USB_CDC
    if (!ev_sink_cdc_ready()) {
        return -1;
    }
    return (ssize_t)mp_usbd_cdc_tx_strn_anytask(buf, len);
#else
    (void)buf;
    (void)len;
    return -1;
#endif
}

static bool ev_sink_uart_ready(void)
{
#if MICROPY_HW_ENABLE_UART_REPL
    return true;
#else
    return false;
#endif
}

static ssize_t ev_sink_uart_write(const void *buf, size_t len)
{
#if MICROPY_HW_ENABLE_UART_REPL
    return (ssize_t)uart_stdout_tx_strn(buf, len);
#else
    (void)buf;
    (void)len;
    return -1;
#endif
}

static bool ev_sink_console_ready(void)
{
    return true;
}

static ssize_t ev_sink_console_write(const void *buf, size_t len)
{
    return (ssize_t)mp_hal_stdout_tx_strn(buf, len);
}

static bool ev_sink_null_ready(void)
{
    return true;
}

static ssize_t ev_sink_null_write(const void *buf, size_t len)
{
    (void)buf;
    return (ssize_t)len;
}

static const ev_sink_t s_sinks[] = {
    { "usj", ev_sink_usj_ready, ev_sink_usj_write },
    { "cdc", ev_sink_cdc_ready, ev_sink_cdc_write },
    { "uart", ev_sink_uart_ready, ev_sink_uart_write },
    { "console", ev_sink_console_ready, ev_sink_console_write },
    { "null", ev_sink_null_ready, ev_sink_null_write },
};

static const ev_sink_t *ev_sink_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < (sizeof(s_sinks) / sizeof(s_sinks[0])); ++i) {
        if (strcmp(id, s_sinks[i].id) == 0) {
            return &s_sinks[i];
        }
    }
    if (strcmp(id, "none") == 0) {
        return ev_sink_find("null");
    }
    return NULL;
}

static bool ev_sink_present(const ev_sink_t *sink)
{
    if (sink == NULL) {
        return false;
    }
    if (strcmp(sink->id, "cdc") == 0) {
        return ev_sink_cdc_present();
    }
    return sink->ready();
}

static const ev_sink_t *ev_default_sink(const char *id)
{
    const ev_sink_t *sink = ev_sink_find(id);
    if ((sink != NULL) && sink->ready()) {
        return sink;
    }
    sink = ev_sink_find("usj");
    if ((sink != NULL) && sink->ready()) {
        return sink;
    }
    sink = ev_sink_find("cdc");
    if ((sink != NULL) && sink->ready()) {
        return sink;
    }
    return ev_sink_find("console");
}

// Called with the route lock held. The callback may write frames (taking a
// per-sink mutex); it must not take the route lock from a different task.
static void ev_channel_notify_route_changed(ev_stream_t stream, const char *reason)
{
    if ((s_route_changed_cb != NULL) && (stream < EV_STREAM_MAX) && (s_routes[stream].sink != NULL)) {
        s_route_changed_cb(stream, s_routes[stream].sink->id, s_routes[stream].auto_route, reason);
    }
}

// The single routing rule of contract v3: when the host holds the USB-OTG
// port open (DTR), both streams live on CDC; otherwise both live on USJ.
// No activation RPC, no lease, no heartbeat — DTR alone decides.
static void ev_channel_auto_route_stream(ev_stream_t stream, const ev_sink_t *cdc)
{
    if ((stream >= EV_STREAM_MAX) || !s_routes[stream].auto_route) {
        return;
    }

    const ev_sink_t *next = ev_default_sink("usj");
    const ev_sink_t *current = s_routes[stream].sink;
    if ((cdc != NULL) && cdc->ready()) {
        next = cdc;
    }
    if ((next != NULL) && (next != current)) {
        s_routes[stream].sink = next;
        const char *reason = (next == cdc) ? "cdc_connected" : "cdc_disconnected";
        if ((current != NULL) && (strcmp(current->id, "null") == 0)) {
            reason = "auto";
        }
        ev_channel_notify_route_changed(stream, reason);
    }
}

void ev_channel_init0(void)
{
    ev_route_lock();
    s_routes[EV_STREAM_USER].sink = ev_default_sink("usj");
    s_routes[EV_STREAM_USER].auto_route = ESP_VISION_AUTO_CDC_ROUTE;
    s_routes[EV_STREAM_DEBUG].sink = ev_default_sink("usj");
    s_routes[EV_STREAM_DEBUG].auto_route = ESP_VISION_AUTO_CDC_ROUTE;
    s_initialized = true;
    ev_route_unlock();
}

static void ev_channel_ensure_init(void)
{
    if (!s_initialized) {
        ev_channel_init0();
    }
}

void ev_channel_poll_auto(void)
{
    ev_channel_ensure_init();
    ev_route_lock();
#if ESP_VISION_AUTO_CDC_ROUTE
    const ev_sink_t *cdc = ev_sink_find("cdc");
    ev_channel_auto_route_stream(EV_STREAM_USER, cdc);
    ev_channel_auto_route_stream(EV_STREAM_DEBUG, cdc);
#endif
    ev_route_unlock();
}

void ev_channel_set_route_changed_cb(ev_channel_route_changed_cb_t callback)
{
    ev_route_lock();
    s_route_changed_cb = callback;
    ev_route_unlock();
}

esp_err_t ev_channel_bind(ev_stream_t stream, const char *sink_id)
{
    ev_channel_ensure_init();
    if (stream >= EV_STREAM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ev_route_lock();
    const ev_sink_t *sink = ev_sink_find(sink_id);
    if (sink == NULL) {
        ev_route_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (!sink->ready() && (strcmp(sink->id, "null") != 0)) {
        ev_route_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_routes[stream].sink = sink;
    s_routes[stream].auto_route = false;
    ev_channel_notify_route_changed(stream, "manual");
    ev_route_unlock();
    return ESP_OK;
}

esp_err_t ev_channel_bind_auto(ev_stream_t stream)
{
    ev_channel_ensure_init();
    if (stream >= EV_STREAM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ev_route_lock();
    s_routes[stream].sink = ev_default_sink("usj");
    s_routes[stream].auto_route = ESP_VISION_AUTO_CDC_ROUTE;
    ev_channel_notify_route_changed(stream, "auto");
    ev_route_unlock();
    return ESP_OK;
}

const char *ev_channel_get(ev_stream_t stream)
{
    ev_channel_ensure_init();
    ev_route_lock();
    const char *id = "null";
    if ((stream < EV_STREAM_MAX) && (s_routes[stream].sink != NULL)) {
        id = s_routes[stream].sink->id;
    }
    ev_route_unlock();
    return id;
}

bool ev_channel_is_auto(ev_stream_t stream)
{
    ev_channel_ensure_init();
    ev_route_lock();
    bool auto_route = (stream < EV_STREAM_MAX) && s_routes[stream].auto_route;
    ev_route_unlock();
    return auto_route;
}

bool ev_channel_ready(ev_stream_t stream)
{
    ev_channel_ensure_init();
    ev_route_lock();
    ev_channel_poll_auto();
    bool ready = (stream < EV_STREAM_MAX) &&
                 (s_routes[stream].sink != NULL) &&
                 s_routes[stream].sink->ready();
    ev_route_unlock();
    return ready;
}

bool ev_channel_sink_ready(const char *sink_id)
{
    ev_channel_ensure_init();
    ev_route_lock();
    const ev_sink_t *sink = ev_sink_find(sink_id);
    bool ready = (sink != NULL) && sink->ready();
    ev_route_unlock();
    return ready;
}

bool ev_channel_sink_present(const char *sink_id)
{
    ev_channel_ensure_init();
    ev_route_lock();
    const ev_sink_t *sink = ev_sink_find(sink_id);
    bool present = ev_sink_present(sink);
    ev_route_unlock();
    return present;
}

const char *ev_channel_sink_state(const char *sink_id)
{
    ev_channel_ensure_init();
    ev_route_lock();
    const ev_sink_t *sink = ev_sink_find(sink_id);
    const char *state = "unknown";
    if (sink != NULL) {
        if (sink->ready()) {
            state = "active";
        } else if (ev_sink_present(sink)) {
            state = "present";
        } else {
            state = "idle";
        }
    }
    ev_route_unlock();
    return state;
}

ssize_t ev_channel_write(ev_stream_t stream, const void *buf, size_t len)
{
    ev_channel_ensure_init();
    if ((stream >= EV_STREAM_MAX) || (buf == NULL) || (len == 0)) {
        return 0;
    }
    // Hold the route lock only to pick the sink; the write itself may block
    // and must not stall route maintenance on other tasks.
    ev_route_lock();
    ev_channel_poll_auto();
    const ev_sink_t *sink = s_routes[stream].sink;
    bool ready = (sink != NULL) && sink->ready();
    ev_route_unlock();
    if (!ready) {
        return -1;
    }
    return sink->write(buf, len);
}

ssize_t ev_channel_write_sink_if_present(const char *sink_id, const void *buf, size_t len)
{
    ev_channel_ensure_init();
    if ((sink_id == NULL) || (buf == NULL) || (len == 0)) {
        return 0;
    }
    const ev_sink_t *sink = ev_sink_find(sink_id);
    if ((sink == NULL) || !ev_sink_present(sink)) {
        return -1;
    }
#if MICROPY_HW_USB_CDC
    if (strcmp(sink->id, "cdc") == 0) {
        return (ssize_t)mp_usbd_cdc_tx_strn_anytask(buf, len);
    }
#endif
    if (!sink->ready()) {
        return -1;
    }
    return sink->write(buf, len);
}

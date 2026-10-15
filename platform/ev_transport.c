/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ev_transport.h"

#include <string.h>
#include <sys/types.h>

#include "boardconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "py/mpconfig.h"
#include "py/ringbuf.h"
#include "ev_transport_config.h"

#if MICROPY_HW_ESP_USB_SERIAL_JTAG
// The generated MicroPython build copy may lag the overlay declaration during
// an incremental syntax check; this symbol is provided by the maintained
// overlay implementation linked into the current port.
extern size_t usb_serial_jtag_tx_some(const char *str, size_t len);
#endif

#if MICROPY_HW_ESP_USB_SERIAL_JTAG
#include "usb_serial_jtag.h"
#endif

#if MICROPY_HW_USB_CDC
#include "shared/tinyusb/mp_usbd.h"
#include "shared/tinyusb/mp_usbd_cdc.h"
#include "tusb.h"
extern mp_uint_t mp_usbd_cdc_tx_strn_anytask(const char *str, mp_uint_t len);
#endif

#if MICROPY_HW_ENABLE_UART_REPL
#include "uart.h"
extern int uart_stdout_tx_some(const char *str, size_t len);
#endif

#if MICROPY_HW_USB_CDC
extern size_t mp_usbd_cdc_tx_some_anytask(const char *str, size_t len);
#endif

extern ringbuf_t stdin_ringbuf;
extern ringbuf_t cdc_stdin_ringbuf;
extern ringbuf_t uart_stdin_ringbuf;

// Implemented by the statically linked TCP backend when this board enables
// TCP. Weak references keep non-network boards independent of socket code.
extern ev_io_result_t ev_tcp_link_read(const ev_link_ref_t *link,
                                       void *buffer,
                                       size_t capacity) __attribute__((weak));
extern ev_io_result_t ev_tcp_link_write(const ev_link_ref_t *link,
                                        const void *buffer,
                                        size_t length) __attribute__((weak));
extern void ev_tcp_link_close(const ev_link_ref_t *link) __attribute__((weak));

typedef struct {
    uint32_t generation;
    bool open;
} ev_transport_link_state_t;

static ev_transport_link_state_t s_links[EV_TRANSPORT_MAX][EV_TRANSPORT_LINK_SLOTS_MAX];
static bool s_supported[EV_TRANSPORT_MAX];
static bool s_initialized;
static bool s_has_owner;
static ev_link_ref_t s_owner;
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
// A serial write and its link-generation transition share a per-endpoint
// mutex. The mutex is held only across one backend's bounded, nonblocking
// write chunk; the registry lock is held only for the final generation check
// or update. TCP socket lifetime remains owned by the TCP manager.
static StaticSemaphore_t s_serial_io_lock_buf[EV_TRANSPORT_TCP];
static SemaphoreHandle_t s_serial_io_lock[EV_TRANSPORT_TCP];

static void ev_transport_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void ev_transport_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static bool ev_transport_serial_io_take(ev_transport_id_t id)
{
    if (id >= EV_TRANSPORT_TCP) {
        return false;
    }
    SemaphoreHandle_t lock = s_serial_io_lock[id];
    return (lock != NULL) && (xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE);
}

static void ev_transport_serial_io_give(ev_transport_id_t id)
{
    if (id < EV_TRANSPORT_TCP) {
        xSemaphoreGive(s_serial_io_lock[id]);
    }
}

static bool ev_transport_valid_slot(ev_transport_id_t id, uint8_t slot)
{
    if (id >= EV_TRANSPORT_MAX) {
        return false;
    }
    return (id == EV_TRANSPORT_TCP) ? (slot < EV_TRANSPORT_TCP_LINKS) : (slot == 0);
}

static uint32_t ev_transport_next_generation(uint32_t generation)
{
    ++generation;
    return generation == 0 ? 1 : generation;
}

static bool ev_transport_id_supported(ev_transport_id_t id)
{
    return (id < EV_TRANSPORT_MAX) && s_supported[id];
}

static bool ev_transport_ref_matches_locked(const ev_link_ref_t *link)
{
    return (link != NULL) && ev_transport_valid_slot(link->transport_id, link->slot) &&
           ev_transport_id_supported(link->transport_id) &&
           s_links[link->transport_id][link->slot].open &&
           (s_links[link->transport_id][link->slot].generation == link->generation);
}

static bool ev_transport_ref_matches(const ev_link_ref_t *link)
{
    if ((link == NULL) || !ev_transport_valid_slot(link->transport_id, link->slot)) {
        return false;
    }
    ev_transport_lock();
    bool matches = ev_transport_ref_matches_locked(link);
    ev_transport_unlock();
    return matches;
}

static ev_io_result_t ev_transport_result(ev_io_status_t status, size_t transferred, esp_err_t error)
{
    ev_io_result_t result = {
        .status = status,
        .transferred = transferred,
        .error = error,
    };
    return result;
}

static bool ev_transport_serial_supported(ev_transport_id_t id)
{
    switch (id) {
    case EV_TRANSPORT_USJ:
#if MICROPY_HW_ESP_USB_SERIAL_JTAG
        return true;
#else
        return false;
#endif
    case EV_TRANSPORT_USB:
#if MICROPY_HW_USB_CDC
        return true;
#else
        return false;
#endif
    case EV_TRANSPORT_UART:
#if MICROPY_HW_ENABLE_UART_REPL
        return true;
#else
        return false;
#endif
    default:
        return false;
    }
}

esp_err_t ev_transport_init0(void)
{
    ev_transport_lock();
    if (s_initialized) {
        ev_transport_unlock();
        return ESP_OK;
    }
    for (ev_transport_id_t id = EV_TRANSPORT_USJ; id < EV_TRANSPORT_TCP; ++id) {
        if (s_serial_io_lock[id] == NULL) {
            s_serial_io_lock[id] = xSemaphoreCreateMutexStatic(&s_serial_io_lock_buf[id]);
        }
        if (s_serial_io_lock[id] == NULL) {
            ev_transport_unlock();
            return ESP_ERR_NO_MEM;
        }
    }
    memset(s_links, 0, sizeof(s_links));
    memset(s_supported, 0, sizeof(s_supported));
    s_supported[EV_TRANSPORT_USJ] = ev_transport_serial_supported(EV_TRANSPORT_USJ);
    s_supported[EV_TRANSPORT_USB] = ev_transport_serial_supported(EV_TRANSPORT_USB);
    s_supported[EV_TRANSPORT_UART] = ev_transport_serial_supported(EV_TRANSPORT_UART);
#if defined(ESP_VISION_ENABLE_TCP_TRANSPORT) && ESP_VISION_ENABLE_TCP_TRANSPORT
    s_supported[EV_TRANSPORT_TCP] = true;
#endif
    for (size_t id = EV_TRANSPORT_USJ; id <= EV_TRANSPORT_UART; ++id) {
        if (s_supported[id]) {
            s_links[id][0].generation = 1;
            s_links[id][0].open = true;
        }
    }
    s_has_owner = false;
    memset(&s_owner, 0, sizeof(s_owner));
    s_initialized = true;
    ev_transport_unlock();
    return ESP_OK;
}

const char *ev_transport_name(ev_transport_id_t id)
{
    static const char *const names[EV_TRANSPORT_MAX] = { "usj", "usb", "uart", "tcp" };
    return (id < EV_TRANSPORT_MAX) ? names[id] : "unknown";
}

bool ev_transport_id_from_name(const char *name, ev_transport_id_t *out_id)
{
    if ((name == NULL) || (out_id == NULL)) {
        return false;
    }
    for (ev_transport_id_t id = EV_TRANSPORT_USJ; id < EV_TRANSPORT_MAX; ++id) {
        if (strcmp(name, ev_transport_name(id)) == 0) {
            *out_id = id;
            return true;
        }
    }
    return false;
}

bool ev_transport_link_ref(ev_transport_id_t id, uint8_t slot, ev_link_ref_t *out_link)
{
    if ((out_link == NULL) || !ev_transport_valid_slot(id, slot)) {
        return false;
    }
    ev_transport_lock();
    bool found = ev_transport_id_supported(id) && s_links[id][slot].open;
    if (found) {
        out_link->transport_id = id;
        out_link->slot = slot;
        out_link->generation = s_links[id][slot].generation;
    }
    ev_transport_unlock();
    return found;
}

bool ev_transport_link_is_current(const ev_link_ref_t *link)
{
    return ev_transport_ref_matches(link);
}

esp_err_t ev_transport_link_open(ev_transport_id_t id, uint8_t slot, ev_link_ref_t *out_link)
{
    if ((out_link == NULL) || !ev_transport_valid_slot(id, slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    ev_transport_lock();
    if (!s_initialized || !ev_transport_id_supported(id)) {
        ev_transport_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    ev_transport_link_state_t *state = &s_links[id][slot];
    if (state->open) {
        ev_transport_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    state->generation = ev_transport_next_generation(state->generation);
    state->open = true;
    out_link->transport_id = id;
    out_link->slot = slot;
    out_link->generation = state->generation;
    ev_transport_unlock();
    return ESP_OK;
}

void ev_transport_link_close(const ev_link_ref_t *link)
{
    if ((link == NULL) || !ev_transport_valid_slot(link->transport_id, link->slot)) {
        return;
    }
    bool serial = link->transport_id < EV_TRANSPORT_TCP;
    if (serial && !ev_transport_serial_io_take(link->transport_id)) {
        return;
    }
    ev_transport_lock();
    if (ev_transport_ref_matches_locked(link)) {
        ev_transport_link_state_t *state = &s_links[link->transport_id][link->slot];
        state->open = false;
        state->generation = ev_transport_next_generation(state->generation);
        if (s_has_owner && (s_owner.transport_id == link->transport_id) &&
                (s_owner.slot == link->slot) && (s_owner.generation == link->generation)) {
            s_has_owner = false;
        }
    }
    ev_transport_unlock();
    if (serial) {
        ev_transport_serial_io_give(link->transport_id);
    }
    if ((link->transport_id == EV_TRANSPORT_TCP) && (ev_tcp_link_close != NULL)) {
        // The TCP backend queues closure to its single socket-manager task;
        // never close a descriptor from the session/TX caller task.
        ev_tcp_link_close(link);
    }
}

bool ev_transport_link_advance_generation(const ev_link_ref_t *link, ev_link_ref_t *out_link)
{
    if ((link == NULL) || (out_link == NULL) || !ev_transport_valid_slot(link->transport_id, link->slot)) {
        return false;
    }
    bool serial = link->transport_id < EV_TRANSPORT_TCP;
    if (serial && !ev_transport_serial_io_take(link->transport_id)) {
        return false;
    }
    ev_transport_lock();
    bool ok = ev_transport_ref_matches_locked(link);
    if (ok) {
        ev_transport_link_state_t *state = &s_links[link->transport_id][link->slot];
        state->generation = ev_transport_next_generation(state->generation);
        *out_link = *link;
        out_link->generation = state->generation;
        if (s_has_owner && (s_owner.transport_id == link->transport_id) && (s_owner.slot == link->slot)) {
            s_owner = *out_link;
        }
    }
    ev_transport_unlock();
    if (serial) {
        ev_transport_serial_io_give(link->transport_id);
    }
    return ok;
}

static int ev_transport_serial_ring_get(ev_transport_id_t id)
{
    switch (id) {
    case EV_TRANSPORT_USJ:
#if MICROPY_HW_ESP_USB_SERIAL_JTAG
        return ringbuf_get(&stdin_ringbuf);
#else
        return -1;
#endif
    case EV_TRANSPORT_USB:
#if MICROPY_HW_USB_CDC
        return ringbuf_get(&cdc_stdin_ringbuf);
#else
        return -1;
#endif
    case EV_TRANSPORT_UART:
#if MICROPY_HW_ENABLE_UART_REPL
        return ringbuf_get(&uart_stdin_ringbuf);
#else
        return -1;
#endif
    default:
        return -1;
    }
}

ev_io_result_t ev_transport_read(const ev_link_ref_t *link, void *buffer, size_t capacity)
{
    if ((buffer == NULL) || (capacity == 0)) {
        return ev_transport_result(EV_IO_ERROR, 0, ESP_ERR_INVALID_ARG);
    }
    if (!ev_transport_ref_matches(link)) {
        return ev_transport_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
    }
    if (link->transport_id == EV_TRANSPORT_TCP) {
        if (ev_tcp_link_read == NULL) {
            return ev_transport_result(EV_IO_ERROR, 0, ESP_ERR_NOT_SUPPORTED);
        }
        return ev_tcp_link_read(link, buffer, capacity);
    }

    size_t read_len = 0;
    uint8_t *bytes = (uint8_t *)buffer;
    while (read_len < capacity) {
        int c = ev_transport_serial_ring_get(link->transport_id);
        if (c < 0) {
            break;
        }
        bytes[read_len++] = (uint8_t)c;
    }
    return (read_len != 0) ? ev_transport_result(EV_IO_PROGRESS, read_len, ESP_OK) :
           ev_transport_result(EV_IO_WOULD_BLOCK, 0, ESP_OK);
}

static ev_io_result_t ev_transport_serial_write(ev_transport_id_t id, const void *buffer, size_t length)
{
    ssize_t written = -1;
    switch (id) {
    case EV_TRANSPORT_USJ:
#if MICROPY_HW_ESP_USB_SERIAL_JTAG
        written = (ssize_t)usb_serial_jtag_tx_some(buffer, length);
#endif
        break;
    case EV_TRANSPORT_USB:
#if MICROPY_HW_USB_CDC
        written = (ssize_t)mp_usbd_cdc_tx_some_anytask((const char *)buffer, length);
#endif
        break;
    case EV_TRANSPORT_UART:
#if MICROPY_HW_ENABLE_UART_REPL
        written = (ssize_t)uart_stdout_tx_some((const char *)buffer, length);
#endif
        break;
    default:
        break;
    }
    if (written > 0) {
        return ev_transport_result(EV_IO_PROGRESS, (size_t)written, ESP_OK);
    }
    if (written == 0) {
        return ev_transport_result(EV_IO_WOULD_BLOCK, 0, ESP_OK);
    }
    return ev_transport_result(EV_IO_WOULD_BLOCK, 0, ESP_OK);
}

ev_io_result_t ev_transport_write(const ev_link_ref_t *link, const void *buffer, size_t length)
{
    if ((buffer == NULL) || (length == 0)) {
        return ev_transport_result(EV_IO_ERROR, 0, ESP_ERR_INVALID_ARG);
    }
    if (link == NULL) {
        return ev_transport_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
    }
    if (link->transport_id == EV_TRANSPORT_TCP) {
        if (!ev_transport_ref_matches(link)) {
            return ev_transport_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
        }
        if (ev_tcp_link_write == NULL) {
            return ev_transport_result(EV_IO_ERROR, 0, ESP_ERR_NOT_SUPPORTED);
        }
        return ev_tcp_link_write(link, buffer, length);
    }
    if ((link->transport_id >= EV_TRANSPORT_TCP) ||
            !ev_transport_serial_io_take(link->transport_id)) {
        return ev_transport_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
    }
    if (!ev_transport_ref_matches(link)) {
        ev_transport_serial_io_give(link->transport_id);
        return ev_transport_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
    }
    ev_io_result_t result = ev_transport_serial_write(link->transport_id, buffer, length);
    ev_transport_serial_io_give(link->transport_id);
    return result;
}

void ev_transport_endpoint_state(ev_transport_id_t id, uint8_t slot,
                                 ev_transport_endpoint_state_t *out_state)
{
    if (out_state == NULL) {
        return;
    }
    memset(out_state, 0, sizeof(*out_state));
    if (!ev_transport_valid_slot(id, slot)) {
        return;
    }
    ev_transport_lock();
    bool supported = ev_transport_id_supported(id);
    bool open = supported && s_links[id][slot].open;
    bool owner = s_has_owner && (s_owner.transport_id == id) && (s_owner.slot == slot) &&
                 (s_owner.generation == s_links[id][slot].generation);
    if ((id == EV_TRANSPORT_TCP) && (slot == EV_TRANSPORT_PHYSICAL_SLOT)) {
        for (uint8_t tcp_slot = 0; tcp_slot < EV_TRANSPORT_TCP_LINKS; ++tcp_slot) {
            open = open || (supported && s_links[id][tcp_slot].open);
            owner = owner || (s_has_owner && (s_owner.transport_id == id) &&
                              (s_owner.slot == tcp_slot) &&
                              (s_owner.generation == s_links[id][tcp_slot].generation));
        }
    }
    ev_transport_unlock();
    out_state->supported = supported;
    out_state->owner = owner;
    if (id == EV_TRANSPORT_TCP) {
        out_state->present_known = true;
        out_state->present = open;
        out_state->connected_known = true;
        out_state->connected = open;
        out_state->ready = open;
        return;
    }
    switch (id) {
    case EV_TRANSPORT_USJ:
#if MICROPY_HW_ESP_USB_SERIAL_JTAG
        out_state->ready = supported;
        out_state->present_known = false;
        out_state->connected_known = false;
#endif
        break;
    case EV_TRANSPORT_USB:
#if MICROPY_HW_USB_CDC
    {
        bool present = false;
        bool mounted = false;
        bool ready = false;
        mp_usbd_cdc_transport_endpoint_state(&present, &mounted, &ready);
        out_state->present_known = true;
        out_state->present = present;
        out_state->connected_known = true;
        out_state->connected = mounted;
        out_state->ready = ready;
    }
#endif
    break;
    case EV_TRANSPORT_UART:
#if MICROPY_HW_ENABLE_UART_REPL
        out_state->present_known = false;
        out_state->connected_known = false;
        out_state->ready = supported;
#endif
        break;
    default:
        break;
    }
}

void ev_transport_set_owner(const ev_link_ref_t *owner_or_null)
{
    ev_transport_lock();
    if ((owner_or_null != NULL) && ev_transport_ref_matches_locked(owner_or_null)) {
        s_owner = *owner_or_null;
        s_has_owner = true;
    } else {
        memset(&s_owner, 0, sizeof(s_owner));
        s_has_owner = false;
    }
    ev_transport_unlock();
}

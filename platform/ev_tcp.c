/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ev_tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "boardconfig.h"
#include "esp_timer.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

#include "ev_control_transport.h"
#include "ev_session.h"
#include "network_service.h"
#include "network_discovery.h"

// The P4 ELF capabilities call chain needs about 7.8 KiB before interrupt and
// context headroom. Keep a 12 KiB static stack and report its
// minimum free bytes through transport.stats for hardware diagnostics.
#define EV_TCP_TASK_STACK_SIZE (12 * 1024)
#define EV_TCP_TASK_PRIORITY (ESP_TASK_PRIO_MIN + 3)
#define EV_TCP_READ_BUDGET (1024)
#define EV_TCP_HANDSHAKE_TIMEOUT_MS (3000)
#define EV_TCP_RECLAIM_IDLE_TIMEOUT_MS (30000)
#define EV_TCP_NETWORK_POLL_MS (100)
#define EV_TCP_MAX_WRITE_CHUNK (4096)

_Static_assert((ESP_VISION_TCP_PORT > 0) && (ESP_VISION_TCP_PORT <= 65535),
               "ESP_VISION_TCP_PORT must be a valid TCP port");

#if ESP_VISION_ENABLE_TCP_TRANSPORT

typedef struct {
    int fd;
    bool active;
    bool claimed_once;
    bool idle_reclaim_armed;
    ev_link_ref_t ref;
    uint32_t accepted_ms;
    uint32_t idle_since_ms;
} ev_tcp_link_t;

static ev_tcp_link_t s_links[EV_TRANSPORT_TCP_LINKS];
static int s_listener = -1;
static char s_listener_address[16];
static uint32_t s_listener_retry_at_ms;

static StaticSemaphore_t s_socket_lock_buf;
static SemaphoreHandle_t s_socket_lock;
static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[EV_TCP_TASK_STACK_SIZE / sizeof(StackType_t)];
static TaskHandle_t s_task;
static portMUX_TYPE s_close_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_close_pending[EV_TRANSPORT_TCP_LINKS];
static uint32_t s_close_generation[EV_TRANSPORT_TCP_LINKS];

static void ev_tcp_task(void *argument);

static TaskHandle_t ev_tcp_task_handle(void)
{
    TaskHandle_t task;
    portENTER_CRITICAL(&s_close_mux);
    task = s_task;
    portEXIT_CRITICAL(&s_close_mux);
    return task;
}

static uint32_t ev_tcp_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static ev_io_result_t ev_tcp_result(ev_io_status_t status, size_t transferred, esp_err_t error)
{
    ev_io_result_t result = {
        .status = status,
        .transferred = transferred,
        .error = error,
    };
    return result;
}

static bool ev_tcp_close_requested(uint8_t slot, uint32_t generation)
{
    bool requested;
    portENTER_CRITICAL(&s_close_mux);
    requested = s_close_pending[slot] && (s_close_generation[slot] == generation);
    portEXIT_CRITICAL(&s_close_mux);
    return requested;
}

static bool ev_tcp_link_matches_locked(const ev_link_ref_t *link)
{
    if ((link == NULL) || (link->transport_id != EV_TRANSPORT_TCP) ||
            (link->slot >= EV_TRANSPORT_TCP_LINKS)) {
        return false;
    }
    const ev_tcp_link_t *state = &s_links[link->slot];
    return state->active && (state->ref.generation == link->generation) &&
           (state->fd >= 0) && !ev_tcp_close_requested(link->slot, link->generation);
}

static bool ev_tcp_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return (flags >= 0) && (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void ev_tcp_mark_close_pending(const ev_link_ref_t *link)
{
    if ((link == NULL) || (link->transport_id != EV_TRANSPORT_TCP) ||
            (link->slot >= EV_TRANSPORT_TCP_LINKS) || (s_socket_lock == NULL) ||
            (xSemaphoreTake(s_socket_lock, portMAX_DELAY) != pdTRUE)) {
        return;
    }
    ev_tcp_link_t *state = &s_links[link->slot];
    if (!state->active || (state->ref.generation != link->generation)) {
        xSemaphoreGive(s_socket_lock);
        return;
    }
    portENTER_CRITICAL(&s_close_mux);
    s_close_pending[link->slot] = true;
    s_close_generation[link->slot] = link->generation;
    portEXIT_CRITICAL(&s_close_mux);
    xSemaphoreGive(s_socket_lock);
    TaskHandle_t task = ev_tcp_task_handle();
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

/* Called by ev_transport after invalidating the generation. The manager task
 * owns socket close and slot reuse; callbacks from TX only enqueue this notice.
 */
void ev_tcp_link_close(const ev_link_ref_t *link)
{
    ev_tcp_mark_close_pending(link);
}

ev_io_result_t ev_tcp_link_write(const ev_link_ref_t *link, const void *buffer, size_t length)
{
    if ((link == NULL) || (buffer == NULL) || (length == 0) ||
            (link->transport_id != EV_TRANSPORT_TCP) ||
            (link->slot >= EV_TRANSPORT_TCP_LINKS) || (s_socket_lock == NULL)) {
        return ev_tcp_result(EV_IO_ERROR, 0, ESP_ERR_INVALID_ARG);
    }
    // Do not wait behind manager socket work. Returning WOULD_BLOCK lets the
    // frame scheduler advance other physical endpoints and retry this link.
    if (xSemaphoreTake(s_socket_lock, 0) != pdTRUE) {
        return ev_tcp_result(EV_IO_WOULD_BLOCK, 0, ESP_OK);
    }
    if (!ev_tcp_link_matches_locked(link)) {
        xSemaphoreGive(s_socket_lock);
        return ev_tcp_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
    }

    size_t request_len = (length > EV_TCP_MAX_WRITE_CHUNK) ? EV_TCP_MAX_WRITE_CHUNK : length;
    int fd = s_links[link->slot].fd;
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    int written = send(fd, buffer, request_len, flags);
    int saved_errno = errno;
    xSemaphoreGive(s_socket_lock);

    if (written > 0) {
        return ev_tcp_result(EV_IO_PROGRESS, (size_t)written, ESP_OK);
    }
    if (written == 0) {
        return ev_tcp_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
    }
    if ((saved_errno == EAGAIN) || (saved_errno == EWOULDBLOCK) || (saved_errno == EINTR)) {
        return ev_tcp_result(EV_IO_WOULD_BLOCK, 0, ESP_OK);
    }
    if ((saved_errno == ECONNRESET) || (saved_errno == ENOTCONN) ||
            (saved_errno == EPIPE) || (saved_errno == ECONNABORTED)) {
        return ev_tcp_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
    }
    return ev_tcp_result(EV_IO_ERROR, 0, ESP_FAIL);
}

ev_io_result_t ev_tcp_link_read(const ev_link_ref_t *link, void *buffer, size_t capacity)
{
    if ((link == NULL) || (buffer == NULL) || (capacity == 0) ||
            (link->transport_id != EV_TRANSPORT_TCP) ||
            (link->slot >= EV_TRANSPORT_TCP_LINKS) || (s_socket_lock == NULL)) {
        return ev_tcp_result(EV_IO_ERROR, 0, ESP_ERR_INVALID_ARG);
    }
    if (xSemaphoreTake(s_socket_lock, 0) != pdTRUE) {
        return ev_tcp_result(EV_IO_WOULD_BLOCK, 0, ESP_OK);
    }
    if (!ev_tcp_link_matches_locked(link)) {
        xSemaphoreGive(s_socket_lock);
        return ev_tcp_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
    }
    size_t read_capacity = (capacity > EV_TCP_READ_BUDGET) ? EV_TCP_READ_BUDGET : capacity;
    int fd = s_links[link->slot].fd;
    int received = recv(fd, buffer, read_capacity, MSG_DONTWAIT);
    int saved_errno = errno;
    xSemaphoreGive(s_socket_lock);

    if (received > 0) {
        return ev_tcp_result(EV_IO_PROGRESS, (size_t)received, ESP_OK);
    }
    if (received == 0) {
        return ev_tcp_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
    }
    if ((saved_errno == EAGAIN) || (saved_errno == EWOULDBLOCK) || (saved_errno == EINTR)) {
        return ev_tcp_result(EV_IO_WOULD_BLOCK, 0, ESP_OK);
    }
    if ((saved_errno == ECONNRESET) || (saved_errno == ENOTCONN) ||
            (saved_errno == EPIPE) || (saved_errno == ECONNABORTED)) {
        return ev_tcp_result(EV_IO_CLOSED, 0, ESP_ERR_INVALID_STATE);
    }
    return ev_tcp_result(EV_IO_ERROR, 0, ESP_FAIL);
}

static void ev_tcp_close_pending_links(void)
{
    for (uint8_t slot = 0; slot < EV_TRANSPORT_TCP_LINKS; ++slot) {
        uint32_t generation = 0;
        bool pending;
        portENTER_CRITICAL(&s_close_mux);
        pending = s_close_pending[slot];
        generation = s_close_generation[slot];
        portEXIT_CRITICAL(&s_close_mux);
        if (!pending) {
            continue;
        }

        if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) == pdTRUE) {
            ev_tcp_link_t *state = &s_links[slot];
            if (state->active && (state->ref.generation == generation)) {
                if (state->fd >= 0) {
                    close(state->fd);
                }
                state->fd = -1;
                state->active = false;
                memset(&state->ref, 0, sizeof(state->ref));
            }
            xSemaphoreGive(s_socket_lock);
        }

        portENTER_CRITICAL(&s_close_mux);
        if (s_close_generation[slot] == generation) {
            s_close_pending[slot] = false;
        }
        portEXIT_CRITICAL(&s_close_mux);
    }
}

static void ev_tcp_request_close_slot(uint8_t slot,
                                      const ev_link_ref_t *expected,
                                      ev_session_change_reason_t reason)
{
    ev_link_ref_t ref;
    bool active = false;
    memset(&ref, 0, sizeof(ref));
    if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_links[slot].active && ((expected == NULL) ||
                                 ((expected->transport_id == EV_TRANSPORT_TCP) && (expected->slot == slot) &&
                                  (s_links[slot].ref.generation == expected->generation)))) {
        ref = s_links[slot].ref;
        active = true;
    }
    xSemaphoreGive(s_socket_lock);
    if (!active) {
        return;
    }

    (void)ev_session_link_lost(&ref, reason);
    ev_transport_link_close(&ref);
    ev_tcp_close_pending_links();
}

static void ev_tcp_close_all_links(ev_session_change_reason_t reason)
{
    for (uint8_t slot = 0; slot < EV_TRANSPORT_TCP_LINKS; ++slot) {
        ev_tcp_request_close_slot(slot, NULL, reason);
    }
}

static int ev_tcp_create_listener(const char *ipv4)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }
    if (!ev_tcp_set_nonblocking(fd)) {
        close(fd);
        return -1;
    }
    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)ESP_VISION_TCP_PORT);
    if (inet_pton(AF_INET, ipv4, &address.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if ((bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) || (listen(fd, 2) != 0)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void ev_tcp_close_listener(void)
{
    esp_vision_discovery_set_listening(false);
    if (s_listener >= 0) {
        close(s_listener);
        s_listener = -1;
    }
    s_listener_address[0] = '\0';
}

static bool ev_tcp_network_should_listen(char ipv4_out[16])
{
    esp_vision_network_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (esp_vision_network_snapshot(&snapshot) != ESP_OK) {
        return false;
    }
    if (!snapshot.supported || !snapshot.enabled ||
            (snapshot.state != ESP_VISION_NETWORK_ONLINE) || (snapshot.ipv4[0] == '\0')) {
        return false;
    }
    memcpy(ipv4_out, snapshot.ipv4, sizeof(snapshot.ipv4));
    ipv4_out[15] = '\0';
    return true;
}

static void ev_tcp_sync_listener(void)
{
    char ipv4[16] = { 0 };
    bool online = ev_tcp_network_should_listen(ipv4);
    bool address_changed = online && (s_listener_address[0] != '\0') &&
                           (strcmp(s_listener_address, ipv4) != 0);
    if (!online || address_changed) {
        ev_tcp_close_listener();
        ev_tcp_close_all_links(EV_SESSION_CHANGE_LINK_LOST);
    }
    if (!online || (s_listener >= 0)) {
        esp_vision_discovery_set_listening(online && s_listener >= 0);
        return;
    }
    uint32_t now_ms = ev_tcp_now_ms();
    if ((int32_t)(now_ms - s_listener_retry_at_ms) < 0) {
        return;
    }
    int listener = ev_tcp_create_listener(ipv4);
    if (listener < 0) {
        s_listener_retry_at_ms = now_ms + 1000;
        return;
    }
    s_listener = listener;
    memcpy(s_listener_address, ipv4, sizeof(s_listener_address));
    s_listener_retry_at_ms = 0;
    esp_vision_discovery_set_listening(true);
}

static int ev_tcp_free_slot(void)
{
    int free_slot = -1;
    if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    for (uint8_t slot = 0; slot < EV_TRANSPORT_TCP_LINKS; ++slot) {
        if (!s_links[slot].active && (s_links[slot].fd < 0)) {
            free_slot = slot;
            break;
        }
    }
    xSemaphoreGive(s_socket_lock);
    return free_slot;
}

static void ev_tcp_accept_one(void)
{
    if (s_listener < 0) {
        return;
    }
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int fd = accept(s_listener, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0) {
        return;
    }
    if (!ev_tcp_set_nonblocking(fd)) {
        close(fd);
        return;
    }

    int enabled = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));

    int slot = ev_tcp_free_slot();
    if (slot < 0) {
        close(fd);
        return;
    }
    ev_link_ref_t ref;
    esp_err_t err = ev_transport_link_open(EV_TRANSPORT_TCP, (uint8_t)slot, &ref);
    if (err != ESP_OK) {
        close(fd);
        return;
    }

    if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) != pdTRUE) {
        ev_transport_link_close(&ref);
        close(fd);
        return;
    }
    ev_tcp_link_t *state = &s_links[slot];
    state->fd = fd;
    state->claimed_once = false;
    state->idle_reclaim_armed = false;
    state->ref = ref;
    state->accepted_ms = ev_tcp_now_ms();
    state->idle_since_ms = 0;
    state->active = true;
    xSemaphoreGive(s_socket_lock);
}

static bool ev_tcp_snapshot_owned_link(const ev_link_ref_t *link)
{
    ev_session_snapshot_t snapshot;
    return ev_session_snapshot(&snapshot) && snapshot.has_owner &&
           (snapshot.owner_link.transport_id == link->transport_id) &&
           (snapshot.owner_link.slot == link->slot) &&
           (snapshot.owner_link.generation == link->generation);
}

static void ev_tcp_poll_link(uint8_t slot)
{
    ev_link_ref_t ref;
    uint32_t accepted_ms;
    bool claimed_once;
    if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!s_links[slot].active) {
        xSemaphoreGive(s_socket_lock);
        return;
    }
    ref = s_links[slot].ref;
    accepted_ms = s_links[slot].accepted_ms;
    claimed_once = s_links[slot].claimed_once;
    xSemaphoreGive(s_socket_lock);

    if (!ev_transport_link_is_current(&ref)) {
        ev_tcp_close_pending_links();
        return;
    }
    if (ev_tcp_snapshot_owned_link(&ref)) {
        if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) == pdTRUE) {
            ev_tcp_link_t *state = &s_links[slot];
            if (state->active && (state->ref.generation == ref.generation)) {
                state->claimed_once = true;
                state->idle_reclaim_armed = false;
            }
            xSemaphoreGive(s_socket_lock);
        }
    } else if (!claimed_once &&
               ((uint32_t)(ev_tcp_now_ms() - accepted_ms) >= EV_TCP_HANDSHAKE_TIMEOUT_MS)) {
        // Preserve the original handshake deadline before reading. A late
        // first hello cannot extend the lifetime of a never-claimed slot.
        ev_tcp_request_close_slot(slot, &ref, EV_SESSION_CHANGE_LINK_LOST);
        return;
    }

    uint8_t rx[EV_TCP_READ_BUDGET];
    bool received_data = false;
    ev_io_result_t result = ev_transport_read(&ref, rx, sizeof(rx));
    if (result.status == EV_IO_PROGRESS) {
        if (result.transferred > 0) {
            received_data = true;
            if (result.transferred <= sizeof(rx)) {
                ev_control_transport_rx(&ref, rx, result.transferred);
            }
        }
    } else if (result.status == EV_IO_WOULD_BLOCK) {
        // Run the per-link parser deadline tick while the socket is idle.
        ev_control_transport_rx(&ref, NULL, 0);
    } else if ((result.status == EV_IO_CLOSED) || (result.status == EV_IO_ERROR)) {
        ev_tcp_request_close_slot(slot, &ref, EV_SESSION_CHANGE_LINK_LOST);
        return;
    }

    // RX is handled before reclamation so a keepalive or same-socket reacquire
    // arriving at the idle deadline refreshes the link before it is considered
    // stale. A live session owner is never reclaimed for receive idleness.
    if (ev_tcp_snapshot_owned_link(&ref)) {
        if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) == pdTRUE) {
            ev_tcp_link_t *state = &s_links[slot];
            if (state->active && (state->ref.generation == ref.generation)) {
                state->claimed_once = true;
                state->idle_reclaim_armed = false;
            }
            xSemaphoreGive(s_socket_lock);
        }
        return;
    }

    bool reclaim = false;
    uint32_t now_ms = ev_tcp_now_ms();
    if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) == pdTRUE) {
        ev_tcp_link_t *state = &s_links[slot];
        if (state->active && (state->ref.generation == ref.generation)) {
            if (state->claimed_once) {
                if (!state->idle_reclaim_armed || received_data) {
                    state->idle_since_ms = now_ms;
                    state->idle_reclaim_armed = true;
                } else {
                    // Unsigned elapsed-time subtraction is safe across millis
                    // wrap because both supported timeouts are far below 2^31 ms.
                    reclaim = (uint32_t)(now_ms - state->idle_since_ms) >=
                              EV_TCP_RECLAIM_IDLE_TIMEOUT_MS;
                }
            } else {
                reclaim = (uint32_t)(now_ms - state->accepted_ms) >=
                          EV_TCP_HANDSHAKE_TIMEOUT_MS;
            }
        }
        xSemaphoreGive(s_socket_lock);
    }
    if (reclaim) {
        ev_tcp_request_close_slot(slot, &ref, EV_SESSION_CHANGE_LINK_LOST);
    }
}

static void ev_tcp_process_external_closes(void)
{
    ev_tcp_close_pending_links();
}

static void ev_tcp_task(void *argument)
{
    (void)argument;
    uint32_t last_network_poll_ms = 0;
    for (;;) {
        uint32_t now_ms = ev_tcp_now_ms();
        if ((uint32_t)(now_ms - last_network_poll_ms) >= EV_TCP_NETWORK_POLL_MS) {
            ev_tcp_sync_listener();
            last_network_poll_ms = now_ms;
        }
        ev_tcp_process_external_closes();

        // One accept and at most 1024 RX bytes per live link per round. A
        // WOULD_BLOCK result is harmless; the loop always yields a tick.
        ev_tcp_accept_one();
        for (uint8_t slot = 0; slot < EV_TRANSPORT_TCP_LINKS; ++slot) {
            ev_tcp_poll_link(slot);
        }
        ev_tcp_process_external_closes();
        vTaskDelay(1);
    }
}

esp_err_t ev_tcp_init0(void)
{
    if (ev_tcp_task_handle() != NULL) {
        return ESP_OK;
    }
    s_socket_lock = xSemaphoreCreateMutexStatic(&s_socket_lock_buf);
    if (s_socket_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t slot = 0; slot < EV_TRANSPORT_TCP_LINKS; ++slot) {
        s_links[slot].fd = -1;
    }
    TaskHandle_t task = xTaskCreateStatic(ev_tcp_task,
                                          "ev_tcp",
                                          EV_TCP_TASK_STACK_SIZE,
                                          NULL,
                                          EV_TCP_TASK_PRIORITY,
                                          s_task_stack,
                                          &s_task_tcb);
    portENTER_CRITICAL(&s_close_mux);
    s_task = task;
    portEXIT_CRITICAL(&s_close_mux);
    return (task != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

uint32_t ev_tcp_task_stack_min_free_bytes(void)
{
    TaskHandle_t task = ev_tcp_task_handle();
    return (task != NULL) ? (uint32_t)uxTaskGetStackHighWaterMark(task) : 0;
}

#else

ev_io_result_t ev_tcp_link_read(const ev_link_ref_t *link, void *buffer, size_t capacity)
{
    (void)link;
    (void)buffer;
    (void)capacity;
    ev_io_result_t result = { .status = EV_IO_ERROR, .transferred = 0, .error = ESP_ERR_NOT_SUPPORTED };
    return result;
}

ev_io_result_t ev_tcp_link_write(const ev_link_ref_t *link, const void *buffer, size_t length)
{
    (void)link;
    (void)buffer;
    (void)length;
    ev_io_result_t result = { .status = EV_IO_ERROR, .transferred = 0, .error = ESP_ERR_NOT_SUPPORTED };
    return result;
}

void ev_tcp_link_close(const ev_link_ref_t *link)
{
    (void)link;
}

esp_err_t ev_tcp_init0(void)
{
    return ESP_OK;
}

uint32_t ev_tcp_task_stack_min_free_bytes(void)
{
    return 0;
}

#endif

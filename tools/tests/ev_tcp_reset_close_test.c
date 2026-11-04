/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "ev_tcp.h"

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 999999
#define pdMS_TO_TICKS(milliseconds) (milliseconds)

/* PRODUCTION_TYPES */

static ev_tcp_link_t s_links[EV_TRANSPORT_TCP_LINKS];
static int mutex_storage;
static int *s_socket_lock = &mutex_storage;
static bool s_close_pending[EV_TRANSPORT_TCP_LINKS];
static uint32_t s_close_generation[EV_TRANSPORT_TCP_LINKS];
static int s_close_mux;
static bool locked, fail_lock;
static int get_error, set_error, option_error, fallback_error, close_error;
static int get_calls, set_calls, option_calls, close_calls;
static int linger_mode = -1, lock_timeout;
static int initial_flags = O_NONBLOCK;
static bool race_pending, race_reuse, expect_reset_linger = true;
static ev_link_ref_t link = { .transport_id = EV_TRANSPORT_TCP, .slot = 0, .generation = 7 };

static int xSemaphoreTake(int *mutex, int timeout)
{
    assert(mutex == s_socket_lock && (timeout == portMAX_DELAY || timeout == 1000));
    lock_timeout = timeout;
    if (fail_lock) {
        return pdFALSE;
    }
    assert(!locked);
    locked = true;
    return pdTRUE;
}

static void xSemaphoreGive(int *mutex)
{
    assert(mutex == s_socket_lock && locked);
    locked = false;
}

#define portENTER_CRITICAL(mux) assert((mux) == &s_close_mux)
#define portEXIT_CRITICAL(mux) assert((mux) == &s_close_mux)

static void ev_tcp_close_pending_links(void);

static int mock_fcntl(int fd, int command, int value)
{
    assert(!locked && fd == 17);
    assert(!s_links[0].active && s_links[0].fd == -1 && s_links[0].ref.generation == 0);
    if (race_pending) {
        race_pending = false;
        s_close_pending[0] = true;
        s_close_generation[0] = link.generation;
        ev_tcp_close_pending_links();
        assert(close_calls == 0);
    }
    if (command == F_GETFL) {
        ++get_calls;
        if (get_error) {
            errno = get_error;
            return -1;
        }
        return initial_flags;
    }
    assert(command == F_SETFL && value == (initial_flags & ~O_NONBLOCK));
    ++set_calls;
    if (set_error) {
        errno = set_error;
        return -1;
    }
    return 0;
}

static int mock_setsockopt(int fd, int level, int name, const void *value, socklen_t size)
{
    assert(!locked && fd == 17 && level == SOL_SOCKET && name == SO_LINGER);
    assert(size == sizeof(struct linger));
    const struct linger *option = value;
    assert(option->l_onoff == 1 && (option->l_linger == 2 || option->l_linger == 0));
    ++option_calls;
    int failure = option->l_linger ? option_error : fallback_error;
    if (failure) {
        errno = failure;
        return -1;
    }
    linger_mode = option->l_linger;
    return 0;
}

static int mock_close(int fd)
{
    assert(fd == 17);
    assert(!locked);
    assert((linger_mode >= 0) == expect_reset_linger);
    assert(!s_links[0].active && s_links[0].fd == -1 && s_links[0].ref.generation == 0);
    ++close_calls;
    assert(close_calls == 1);
    if (race_reuse) {
        race_reuse = false;
        s_links[0].active = true;
        s_links[0].fd = 88;
        s_links[0].ref = link;
        ++s_links[0].ref.generation;
        s_close_pending[0] = true;
        s_close_generation[0] = link.generation;
        ev_tcp_close_pending_links();
        assert(s_links[0].active && s_links[0].fd == 88);
    }
    if (close_error) {
        errno = close_error;
        return -1;
    }
    return 0;
}

#define fcntl mock_fcntl
#define setsockopt mock_setsockopt
#define close mock_close

/* PRODUCTION_TCP_FUNCTIONS */

typedef unsigned ev_mux_ticket_t;
static ev_mux_ticket_t *s_response_ticket_out;
static ev_link_ref_t s_response_link;
static esp_err_t enqueue_result = ESP_OK, wait_result = ESP_OK, transmit_result = ESP_OK;
static bool no_ticket, advance_generation;
static int waits, forgets, restarts, logs;
static char last_log[256];

static esp_err_t ev_control_send_rpc_with_seq(const char *type, const char *method,
                                              const char *payload, uint32_t seq)
{
    assert(strcmp(type, "rsp") == 0 && strcmp(method, "device.control") == 0);
    assert(strcmp(payload, "{\"ok\":true,\"action\":\"reset\"}") == 0 && seq == 23);
    assert(s_response_ticket_out != NULL);
    if ((enqueue_result == ESP_OK) && !no_ticket) {
        *s_response_ticket_out = 42;
    }
    return enqueue_result;
}

static esp_err_t ev_mux_wait(ev_mux_ticket_t ticket, uint32_t timeout, esp_err_t *result)
{
    assert(s_response_ticket_out == NULL && ticket == 42 && timeout == 1000);
    ++waits;
    if (wait_result == ESP_OK) {
        *result = transmit_result;
    }
    if (advance_generation) {
        ++s_links[0].ref.generation;
    }
    return wait_result;
}

static void ev_mux_ticket_forget(ev_mux_ticket_t ticket)
{
    assert(ticket == 42 && waits == 1);
    ++forgets;
}

static void mock_log(const char *tag, const char *format, ...)
{
    assert(strcmp(tag, "ev_control") == 0 && s_response_ticket_out == NULL);
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
    ++logs;
}

#define ESP_LOGW(...) mock_log(__VA_ARGS__)

static void esp_restart(void)
{
    assert(!locked && s_response_ticket_out == NULL);
    ++restarts;
    assert(restarts == 1);
}

/* PRODUCTION_RESET_FUNCTION */

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    s_links[0].active = true;
    s_links[0].fd = 17;
    s_links[0].ref = link;
    s_response_link = link;
    ev_tcp_reset_close_result_t result;
    memset(&result, 0xa5, sizeof(result));

    if (strcmp(scenario, "success") == 0) {
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_OK);
        assert(result.detached && result.linger_configured && !result.setup_errno && !result.close_errno);
        assert(result.close_attempted && !result.abort_configured && !result.fallback_errno);
        assert(close_calls == 1 && get_calls == 1 && set_calls == 1 && option_calls == 1 && !locked);
    } else if (strcmp(scenario, "already_blocking") == 0) {
        initial_flags = 0;
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_OK);
        assert(set_calls == 0 && option_calls == 1 && close_calls == 1);
    } else if (strcmp(scenario, "stale_generation") == 0) {
        ++link.generation;
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_ERR_INVALID_STATE);
        assert(!result.detached && !get_calls && !close_calls && s_links[0].active);
    } else if (strcmp(scenario, "invalid_arguments") == 0) {
        assert(ev_tcp_link_close_for_reset(&link, NULL) == ESP_ERR_INVALID_ARG);
        assert(ev_tcp_link_close_for_reset(NULL, &result) == ESP_ERR_INVALID_ARG);
        assert(!result.detached);
        link.slot = EV_TRANSPORT_TCP_LINKS;
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_ERR_INVALID_ARG);
        link.slot = 0;
        link.transport_id = EV_TRANSPORT_USB;
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_ERR_INVALID_ARG);
        link.transport_id = EV_TRANSPORT_TCP;
        s_socket_lock = NULL;
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_ERR_INVALID_ARG);
        assert(close_calls == 0 && get_calls == 0);
    } else if (strcmp(scenario, "lock_failure") == 0) {
        fail_lock = true;
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_ERR_TIMEOUT);
        assert(lock_timeout == 1000);
        assert(!result.detached && close_calls == 0 && s_links[0].active);
    } else if (strcmp(scenario, "get_flags_failure") == 0 ||
               strcmp(scenario, "set_flags_failure") == 0 ||
               strcmp(scenario, "linger_failure") == 0 ||
               strcmp(scenario, "close_failure") == 0 ||
               strcmp(scenario, "setup_and_close_failure") == 0) {
        if (strcmp(scenario, "get_flags_failure") == 0) {
            get_error = EIO;
        }
        if (strcmp(scenario, "set_flags_failure") == 0) {
            set_error = EPERM;
        }
        if (strcmp(scenario, "linger_failure") == 0) {
            option_error = ENOPROTOOPT;
        }
        if (strcmp(scenario, "close_failure") == 0) {
            close_error = ECONNRESET;
        }
        if (strcmp(scenario, "setup_and_close_failure") == 0) {
            get_error = EIO;
            close_error = EBADF;
        }
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_FAIL);
        assert(result.detached && close_calls == 1);
        assert(result.setup_errno == (get_error ? get_error : (set_error ? set_error : option_error)));
        assert(result.close_errno == close_error);
        assert(result.linger_configured == (!get_error && !set_error && !option_error));
        assert(result.abort_configured == (!result.linger_configured));
        assert(result.close_attempted && !result.fallback_errno);
        assert(option_calls == (get_error || set_error ? 1 : (option_error ? 2 : 1)));
    } else if (strcmp(scenario, "fallback_failure") == 0) {
        option_error = ENOPROTOOPT;
        fallback_error = EACCES;
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_FAIL);
        assert(result.detached && !result.linger_configured && !result.abort_configured);
        assert(!result.close_attempted && !result.close_errno && close_calls == 0);
        assert(result.setup_errno == ENOPROTOOPT && result.fallback_errno == EACCES);
    } else if (strcmp(scenario, "pending_close_wins") == 0) {
        s_close_pending[0] = true;
        s_close_generation[0] = link.generation;
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_ERR_INVALID_STATE);
        assert(!result.detached && close_calls == 0);
        expect_reset_linger = false;
        ev_tcp_close_pending_links();
        assert(close_calls == 1 && get_calls == 0 && option_calls == 0);
    } else if (strcmp(scenario, "late_pending_close") == 0) {
        race_pending = true;
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_OK);
        assert(close_calls == 1 && !s_close_pending[0]);
    } else if (strcmp(scenario, "slot_reused_during_close") == 0) {
        race_reuse = true;
        assert(ev_tcp_link_close_for_reset(&link, &result) == ESP_OK);
        assert(close_calls == 1 && s_links[0].active && s_links[0].fd == 88);
        assert(s_links[0].ref.generation == link.generation + 1);
    } else if (strcmp(scenario, "ordinary_close_unchanged") == 0) {
        s_close_pending[0] = true;
        s_close_generation[0] = link.generation;
        expect_reset_linger = false;
        ev_tcp_close_pending_links();
        assert(close_calls == 1 && get_calls == 0 && option_calls == 0 && !s_links[0].active);
    } else if (strncmp(scenario, "reset_", 6) == 0) {
        esp_err_t expected = ESP_OK;
        bool tcp_close = true;
        if (strcmp(scenario, "reset_enqueue_failure") == 0) {
            enqueue_result = ESP_FAIL;
            expected = ESP_FAIL;
            tcp_close = false;
        } else if (strcmp(scenario, "reset_missing_ticket") == 0) {
            no_ticket = true;
            expected = ESP_ERR_INVALID_STATE;
            tcp_close = false;
        } else if (strcmp(scenario, "reset_wait_timeout") == 0) {
            wait_result = ESP_ERR_TIMEOUT;
            expected = ESP_ERR_TIMEOUT;
            tcp_close = false;
        } else if (strcmp(scenario, "reset_transmit_failure") == 0) {
            transmit_result = ESP_FAIL;
            expected = ESP_FAIL;
            tcp_close = false;
        } else if (strcmp(scenario, "reset_stale_generation") == 0) {
            advance_generation = true;
            expected = ESP_ERR_INVALID_STATE;
            tcp_close = false;
        } else if (strcmp(scenario, "reset_local_setup_failure") == 0) {
            option_error = ENOPROTOOPT;
            expected = ESP_FAIL;
        } else if (strcmp(scenario, "reset_local_close_failure") == 0) {
            close_error = ECONNRESET;
            expected = ESP_FAIL;
        } else if (strcmp(scenario, "reset_fallback_failure") == 0) {
            option_error = ENOPROTOOPT;
            fallback_error = EACCES;
            expected = ESP_FAIL;
            tcp_close = false;
        } else if (strcmp(scenario, "reset_usj") == 0) {
            s_response_link.transport_id = EV_TRANSPORT_USJ;
            tcp_close = false;
        } else if (strcmp(scenario, "reset_usb") == 0) {
            s_response_link.transport_id = EV_TRANSPORT_USB;
            tcp_close = false;
        } else if (strcmp(scenario, "reset_uart") == 0) {
            s_response_link.transport_id = EV_TRANSPORT_UART;
            tcp_close = false;
        } else {
            assert(strcmp(scenario, "reset_tcp") == 0);
        }
        assert(ev_control_send_reset(23) == expected);
        assert(restarts == 1 && s_response_ticket_out == NULL);
        assert(close_calls == (tcp_close ? 1 : 0));
        assert(waits == ((enqueue_result == ESP_OK && !no_ticket) ? 1 : 0) && waits == forgets);
        assert(logs == (expected != ESP_OK ? 1 : 0));
        if (wait_result != ESP_OK || transmit_result != ESP_OK || enqueue_result != ESP_OK || no_ticket) {
            assert(strstr(last_log, "response unconfirmed") != NULL);
        }
    } else {
        assert(!"unknown scenario");
    }
    assert(!locked);
    return 0;
}

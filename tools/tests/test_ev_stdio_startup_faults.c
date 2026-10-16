/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdarg.h>
#include <string.h>

#include "../../platform/ev_stdio.c"

int64_t ev_mux_test_time_us;

static esp_err_t s_mux_init_result;
static unsigned s_mux_init_calls;
static unsigned s_control_init_calls;
static unsigned s_control_start_calls;
static unsigned s_vm_reset_begin_calls;
static unsigned s_hello_calls;
static unsigned s_previous_log_calls;
static vprintf_like_t s_log_handler;

static int previous_log_vprintf(const char *format, va_list args)
{
    (void)format;
    (void)args;
    ++s_previous_log_calls;
    return 23;
}

static int call_vprintf(vprintf_like_t function, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = function(format, args);
    va_end(args);
    return result;
}

vprintf_like_t esp_log_set_vprintf(vprintf_like_t function)
{
    vprintf_like_t previous = s_log_handler;
    s_log_handler = function;
    return previous;
}

esp_err_t ev_mux_init0(void)
{
    ++s_mux_init_calls;
    return s_mux_init_result;
}

void ev_control_transport_init0(void)
{
    ++s_control_init_calls;
}

void ev_control_transport_start(void)
{
    ++s_control_start_calls;
}

void ev_control_transport_vm_reset_begin(void)
{
    ++s_vm_reset_begin_calls;
}

esp_err_t ev_control_transport_send_hello(void)
{
    ++s_hello_calls;
    return ESP_OK;
}

esp_err_t ev_mux_write_class(ev_stream_t stream,
                             ev_mux_tx_class_t tx_class,
                             const char *metadata,
                             const void *payload,
                             size_t payload_len)
{
    (void)stream;
    (void)tx_class;
    (void)metadata;
    (void)payload;
    (void)payload_len;
    return ESP_OK;
}

void ev_mux_record_tx_drop(void)
{
}

int main(void)
{
    s_log_handler = previous_log_vprintf;
    s_mux_init_result = ESP_ERR_NO_MEM;

    assert(ev_stdio_init0() == ESP_ERR_NO_MEM);
    assert(s_mux_init_calls == 1);
    assert(s_control_init_calls == 0);
    assert(s_hello_calls == 0);
    assert(!ev_stdio_mux_enabled());

    ev_stdio_vm_reset_begin();
    assert(s_vm_reset_begin_calls == 0);
    ev_stdio_start_transport();
    ev_stdio_set_mux_enabled(true);
    assert(s_control_start_calls == 0);
    assert(!ev_stdio_mux_enabled());

    assert(call_vprintf(s_log_handler, "startup diagnostic") == 23);
    assert(s_previous_log_calls == 1);

    // A later VM reset retries initialization and enables the control plane
    // only after the transport storage is available.
    s_mux_init_result = ESP_OK;
    assert(ev_stdio_init0() == ESP_OK);
    assert(s_mux_init_calls == 2);
    assert(s_control_init_calls == 1);
    assert(s_hello_calls == 1);
    assert(ev_stdio_mux_enabled());

    ev_stdio_vm_reset_begin();
    assert(s_vm_reset_begin_calls == 1);
    ev_stdio_start_transport();
    assert(s_control_start_calls == 1);
    return 0;
}

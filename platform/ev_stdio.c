/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ev_stdio.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/reent.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ev_channel.h"
#include "ev_control_transport.h"
#include "ev_mux.h"

#define EV_STDIO_PAYLOAD_BUF_SIZE (384)
#define EV_STDIO_METADATA_BUF_SIZE (160)

static vprintf_like_t s_prev_log_vprintf;
typedef int (*ev_stdio_newlib_write_fn_t)(struct _reent *r, void *cookie, const char *buf, int len);
static ev_stdio_newlib_write_fn_t s_prev_stdout_write;
static ev_stdio_newlib_write_fn_t s_prev_stderr_write;
volatile bool ev_stdio_mux_enabled_flag;
static bool s_log_hook_installed;
static bool s_stdio_hook_installed;
static uint32_t s_stdio_seq;
static __thread bool s_in_stdio_hook;

static uint32_t ev_stdio_next_seq(void)
{
    return __atomic_fetch_add(&s_stdio_seq, 1, __ATOMIC_RELAXED);
}

static uint32_t ev_stdio_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool ev_stdio_write_mux_text(const char *channel, const char *method, const char *str, size_t len)
{
    if (!ev_stdio_mux_enabled_flag || (str == NULL) || (len == 0)) {
        return false;
    }
    if (s_in_stdio_hook) {
        return true;
    }

    s_in_stdio_hook = true;
    char metadata[EV_STDIO_METADATA_BUF_SIZE];
    uint32_t seq = ev_stdio_next_seq();
    uint32_t ts_ms = ev_stdio_now_ms();
    int metadata_len;
    if (method != NULL) {
        metadata_len = snprintf(metadata,
                                sizeof(metadata),
                                "{\"sid\":\"stdio\",\"seq\":%" PRIu32 ",\"channel\":\"%s\","
                                "\"type\":\"data\",\"method\":\"%s\",\"encoding\":\"text\",\"ts_ms\":%" PRIu32 "}",
                                seq,
                                channel,
                                method,
                                ts_ms);
    } else {
        metadata_len = snprintf(metadata,
                                sizeof(metadata),
                                "{\"sid\":\"stdio\",\"seq\":%" PRIu32 ",\"channel\":\"%s\","
                                "\"type\":\"data\",\"encoding\":\"text\",\"ts_ms\":%" PRIu32 "}",
                                seq,
                                channel,
                                ts_ms);
    }

    if ((metadata_len > 0) && ((size_t)metadata_len < sizeof(metadata))) {
        ev_stream_t stream = (strcmp(channel, "log.idf") == 0) ? EV_STREAM_DEBUG : EV_STREAM_USER;
        (void)ev_mux_write(stream, metadata, str, len);
    }
    s_in_stdio_hook = false;
    return true;
}

static int ev_stdio_log_vprintf(const char *fmt, va_list ap)
{
    if (!ev_stdio_mux_enabled_flag) {
        if (s_prev_log_vprintf != NULL) {
            return s_prev_log_vprintf(fmt, ap);
        }
        return vprintf(fmt, ap);
    }
    if (s_in_stdio_hook) {
        return 0;
    }

    char payload[EV_STDIO_PAYLOAD_BUF_SIZE];
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int len = vsnprintf(payload, sizeof(payload), fmt, ap_copy);
    va_end(ap_copy);
    if (len <= 0) {
        return len;
    }

    size_t write_len = (size_t)len;
    if (write_len >= sizeof(payload)) {
        write_len = sizeof(payload) - 1;
    }
    ev_stdio_write_mux_text("log.idf", "esp-idf", payload, write_len);
    return len;
}

static int ev_stdio_fallback_write(ev_stdio_newlib_write_fn_t fn,
                                   struct _reent *r,
                                   void *cookie,
                                   const char *buf,
                                   int len)
{
    if (fn != NULL) {
        return fn(r, cookie, buf, len);
    }
    return len;
}

static int ev_stdio_stdout_write(struct _reent *r, void *cookie, const char *buf, int len)
{
    if ((len <= 0) || !ev_stdio_write_c_stdout(buf, (size_t)len)) {
        return ev_stdio_fallback_write(s_prev_stdout_write, r, cookie, buf, len);
    }
    return len;
}

static int ev_stdio_stderr_write(struct _reent *r, void *cookie, const char *buf, int len)
{
    if ((len <= 0) || !ev_stdio_write_c_stderr(buf, (size_t)len)) {
        return ev_stdio_fallback_write(s_prev_stderr_write, r, cookie, buf, len);
    }
    return len;
}

// When enabled (default), the EV-MUX control plane is up from boot: hosts
// never need raw REPL access to establish the protocol. sensor.evmux()
// remains as a debug toggle. Disable per-board in boardconfig.h to restore
// the legacy opt-in behaviour.
#ifndef ESP_VISION_EV_MUX_DEFAULT_ENABLED
#define ESP_VISION_EV_MUX_DEFAULT_ENABLED (1)
#endif

void ev_stdio_init0(void)
{
    // Pause physical-ingress parsing while soft-reset state is rebuilt.
    ev_stdio_mux_enabled_flag = false;
    (void)ev_mux_init0();
    ev_control_transport_init0();
    if (!s_log_hook_installed) {
        s_prev_log_vprintf = esp_log_set_vprintf(ev_stdio_log_vprintf);
        s_log_hook_installed = true;
    }
#if CONFIG_LIBC_NEWLIB
    if (!s_stdio_hook_installed) {
        s_prev_stdout_write = stdout->_write;
        s_prev_stderr_write = stderr->_write;
        stdout->_write = ev_stdio_stdout_write;
        stderr->_write = ev_stdio_stderr_write;
        s_stdio_hook_installed = true;
    }
#endif
#if ESP_VISION_EV_MUX_DEFAULT_ENABLED
    // Deterministic state on every (soft) reset: mux on and routes at their
    // defaults. The boot hello is best-effort here (CDC is not up on a
    // cold boot); the transport task re-emits hello on the CDC connect edge.
    ev_stdio_mux_enabled_flag = true;
    (void)ev_control_transport_send_hello();
#else
    ev_stdio_mux_enabled_flag = false;
#endif
}

void ev_stdio_start_transport(void)
{
    ev_control_transport_start();
}

void ev_stdio_set_mux_enabled(bool enabled)
{
    bool was_enabled = ev_stdio_mux_enabled_flag;
    ev_stdio_mux_enabled_flag = enabled;
    if (enabled && !was_enabled) {
        (void)ev_control_transport_send_hello();
    }
}

bool ev_stdio_mux_enabled(void)
{
    return ev_stdio_mux_enabled_flag;
}

bool ev_stdio_write_repl_stdout(const char *str, size_t len)
{
    return ev_stdio_write_mux_text("repl.stdout", NULL, str, len);
}

bool ev_stdio_write_repl_stderr(const char *str, size_t len)
{
    return ev_stdio_write_mux_text("repl.stderr", NULL, str, len);
}

bool ev_stdio_write_c_stdout(const char *str, size_t len)
{
    return ev_stdio_write_mux_text("repl.stdout", "c-stdio", str, len);
}

bool ev_stdio_write_c_stderr(const char *str, size_t len)
{
    return ev_stdio_write_mux_text("repl.stderr", "c-stdio", str, len);
}

bool ev_stdio_write_idf_log(const char *str, size_t len)
{
    return ev_stdio_write_mux_text("log.idf", "esp-vision", str, len);
}

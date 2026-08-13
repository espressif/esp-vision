/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void ev_stdio_init0(void);
void ev_stdio_start_transport(void);
void ev_stdio_set_mux_enabled(bool enabled);
bool ev_stdio_mux_enabled(void);

// ISR-safe mirror of the EV-MUX switch. Low-level RX paths (including IRAM
// interrupt handlers) must read this flag directly instead of calling
// ev_stdio_mux_enabled(). While it is set they must not intercept
// mp_interrupt_char: the byte stream carries EV-MUX frames, and interrupt
// semantics belong to the framed repl.signal channel.
extern volatile bool ev_stdio_mux_enabled_flag;

bool ev_stdio_write_repl_stdout(const char *str, size_t len);
bool ev_stdio_write_repl_stderr(const char *str, size_t len);
bool ev_stdio_write_c_stdout(const char *str, size_t len);
bool ev_stdio_write_c_stderr(const char *str, size_t len);
bool ev_stdio_write_idf_log(const char *str, size_t len);

#ifdef __cplusplus
}
#endif

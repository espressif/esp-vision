/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_CONTROL_INGRESS_USJ = 0,
    EV_CONTROL_INGRESS_CDC,
    EV_CONTROL_INGRESS_UART,
    EV_CONTROL_INGRESS_MAX,
} ev_control_ingress_t;

void ev_control_transport_init0(void);
esp_err_t ev_control_transport_send_hello(void);

// VM-task entry points (mp_hal_stdin_rx_chr / mp_hal_stdio_poll only).
// Frame reception, parsing and route maintenance run on the dedicated
// transport task; these functions only bridge results into VM context.
bool ev_control_transport_read_pending(int *out_chr);
bool ev_control_transport_repl_pending(void);
void ev_control_transport_vm_poll(void);

#ifdef __cplusplus
}
#endif

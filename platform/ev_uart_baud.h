/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EV_UART_BAUD_BASELINE (115200U)
#define EV_UART_BAUD_TIMEOUT_MS (5000U)
#define EV_UART_BAUD_SWITCH_DELAY_MS (100U)
#define EV_UART_BAUD_PROBE_BYTES (512U)
#define EV_UART_BAUD_TOKEN_MAX (32U)

// Transport-independent state machine. The control transport serializes access;
// TX callbacks only mark acknowledgement completion, never touch the hardware.
typedef enum {
    EV_UART_BAUD_IDLE = 0,
    EV_UART_BAUD_PREPARE_ACK,
    EV_UART_BAUD_WAIT_SWITCH,
    EV_UART_BAUD_PROBING,
    EV_UART_BAUD_COMMIT_ACK,
    EV_UART_BAUD_ACTIVE,
    EV_UART_BAUD_RESTORE,
} ev_uart_baud_phase_t;

typedef struct {
    ev_uart_baud_phase_t phase;
    uint32_t requested;
    uint32_t since_ms;
    bool probed;
    char token[EV_UART_BAUD_TOKEN_MAX + 1];
} ev_uart_baud_t;

static inline bool ev_uart_baud_candidate(uint32_t baud)
{
    return (baud == 2000000U) || (baud == 1500000U) || (baud == 921600U) ||
           (baud == 460800U) || (baud == EV_UART_BAUD_BASELINE);
}

static inline bool ev_uart_baud_token_valid(const char *token)
{
    if ((token == NULL) || (token[0] == '\0')) {
        return false;
    }
    size_t length = 0;
    for (; token[length] != '\0'; ++length) {
        char c = token[length];
        if ((length >= EV_UART_BAUD_TOKEN_MAX) ||
                !(((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
                  ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-'))) {
            return false;
        }
    }
    return true;
}

static inline bool ev_uart_baud_prepare(ev_uart_baud_t *state, uint32_t baud,
                                        const char *token, uint32_t now_ms)
{
    if ((state->phase != EV_UART_BAUD_IDLE && state->phase != EV_UART_BAUD_ACTIVE) ||
            !ev_uart_baud_candidate(baud) || !ev_uart_baud_token_valid(token)) {
        return false;
    }
    *state = (ev_uart_baud_t) {
        .phase = EV_UART_BAUD_PREPARE_ACK, .requested = baud, .since_ms = now_ms,
    };
    memcpy(state->token, token, strlen(token) + 1);
    return true;
}

static inline void ev_uart_baud_prepare_complete(ev_uart_baud_t *state, const char *token,
                                                 bool success, uint32_t now_ms)
{
    if ((state->phase == EV_UART_BAUD_PREPARE_ACK) && (strcmp(state->token, token) == 0)) {
        state->phase = success ? EV_UART_BAUD_WAIT_SWITCH : EV_UART_BAUD_RESTORE;
        state->since_ms = now_ms;
    }
}

static inline bool ev_uart_baud_switch_due(const ev_uart_baud_t *state, uint32_t now_ms)
{
    return (state->phase == EV_UART_BAUD_WAIT_SWITCH) &&
           ((uint32_t)(now_ms - state->since_ms) >= EV_UART_BAUD_SWITCH_DELAY_MS);
}

static inline bool ev_uart_baud_expired(const ev_uart_baud_t *state, uint32_t now_ms)
{
    return (state->phase != EV_UART_BAUD_IDLE) && (state->phase != EV_UART_BAUD_ACTIVE) &&
           (state->phase != EV_UART_BAUD_RESTORE) &&
           ((uint32_t)(now_ms - state->since_ms) >= EV_UART_BAUD_TIMEOUT_MS);
}

static inline bool ev_uart_baud_matches(const ev_uart_baud_t *state, const char *token)
{
    return ((state->phase == EV_UART_BAUD_PROBING) || (state->phase == EV_UART_BAUD_ACTIVE)) &&
           (strcmp(state->token, token) == 0);
}

static inline bool ev_uart_baud_commit(ev_uart_baud_t *state, const char *token)
{
    if (!ev_uart_baud_matches(state, token) || !state->probed) {
        return false;
    }
    if (state->phase != EV_UART_BAUD_ACTIVE) {
        state->phase = EV_UART_BAUD_COMMIT_ACK;
    }
    return true;
}

static inline void ev_uart_baud_commit_complete(ev_uart_baud_t *state, const char *token, bool success)
{
    if ((state->phase == EV_UART_BAUD_COMMIT_ACK) && (strcmp(state->token, token) == 0)) {
        state->phase = success ? EV_UART_BAUD_ACTIVE : EV_UART_BAUD_RESTORE;
    }
}

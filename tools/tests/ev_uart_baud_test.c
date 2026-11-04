/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdio.h>
#include "ev_uart_baud.h"

int main(void)
{
    ev_uart_baud_t state = {0};
    const uint32_t candidates[] = {2000000, 1500000, 921600, 460800, 115200};
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        assert(ev_uart_baud_candidate(candidates[i]));
    }
    assert(!ev_uart_baud_candidate(9600));
    assert(!ev_uart_baud_token_valid(""));
    assert(!ev_uart_baud_token_valid("bad\"token"));
    assert(!ev_uart_baud_token_valid("123456789012345678901234567890123"));
    assert(!ev_uart_baud_prepare(&state, 9600, "test", 10));
    assert(ev_uart_baud_prepare(&state, 2000000, "test", 10));
    assert(!ev_uart_baud_prepare(&state, 1500000, "other", 20));
    assert(!ev_uart_baud_matches(&state, "test"));
    ev_uart_baud_prepare_complete(&state, "wrong", true, 20);
    assert(state.phase == EV_UART_BAUD_PREPARE_ACK);
    ev_uart_baud_prepare_complete(&state, "test", true, 20);
    assert(!ev_uart_baud_switch_due(&state, 119));
    assert(ev_uart_baud_switch_due(&state, 120));
    assert(!ev_uart_baud_expired(&state, 5019));
    assert(ev_uart_baud_expired(&state, 5020));
    state.phase = EV_UART_BAUD_PROBING;
    assert(!ev_uart_baud_commit(&state, "test"));
    assert(ev_uart_baud_matches(&state, "test"));
    state.probed = true;
    assert(!ev_uart_baud_commit(&state, "wrong"));
    assert(ev_uart_baud_commit(&state, "test"));
    assert(state.phase == EV_UART_BAUD_COMMIT_ACK);
    ev_uart_baud_commit_complete(&state, "wrong", true);
    assert(state.phase == EV_UART_BAUD_COMMIT_ACK);
    ev_uart_baud_commit_complete(&state, "test", true);
    assert(state.phase == EV_UART_BAUD_ACTIVE);
    assert(!ev_uart_baud_expired(&state, 30000));
    assert(ev_uart_baud_matches(&state, "test"));
    assert(ev_uart_baud_commit(&state, "test")); // Lost commit ACK retry.
    assert(state.phase == EV_UART_BAUD_ACTIVE);
    assert(ev_uart_baud_prepare(&state, 115200, "fallback", UINT32_MAX - 50));
    assert(!ev_uart_baud_expired(&state, 100)); // Millisecond clock wrap.
    ev_uart_baud_prepare_complete(&state, "fallback", false, 100);
    assert(state.phase == EV_UART_BAUD_RESTORE);
    state = (ev_uart_baud_t) {
        0
    };
    assert(ev_uart_baud_prepare(&state, 115200, "baseline", 0));
    ev_uart_baud_prepare_complete(&state, "baseline", true, UINT32_MAX - 50);
    assert(ev_uart_baud_switch_due(&state, 50));
    assert(ev_uart_baud_expired(&state, 5000));
    state.phase = EV_UART_BAUD_PROBING;
    state.probed = true;
    assert(ev_uart_baud_commit(&state, "baseline"));
    ev_uart_baud_commit_complete(&state, "baseline", false);
    assert(state.phase == EV_UART_BAUD_RESTORE);
    puts("UART baud transition fault tests passed");
}

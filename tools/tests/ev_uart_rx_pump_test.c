/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
// Included after the extracted production pump by test_ev_uart_baud.py.
int main(void)
{
    remaining[EV_TRANSPORT_USJ] = 768;
    remaining[EV_TRANSPORT_USB] = 768;
    remaining[EV_TRANSPORT_UART] = 1536;
    ev_control_transport_pump();
    assert(received[EV_TRANSPORT_USJ] == 256);
    assert(received[EV_TRANSPORT_USB] == 256);
    assert(received[EV_TRANSPORT_UART] == 1024);
    ev_control_transport_pump();
    assert(received[EV_TRANSPORT_UART] == 1536);
    assert(received[EV_TRANSPORT_USB] == 512);
    assert(expired[EV_TRANSPORT_UART] == 1);
    puts("UART sustained ingress and endpoint fairness tests passed");
}

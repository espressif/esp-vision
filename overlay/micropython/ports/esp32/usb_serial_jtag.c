/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Patrick Van Oosterwijck
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// ESP-VISION overlay: identical to the upstream v1.28.0 file except that the
// mp_interrupt_char interception is disabled while EV-MUX owns the byte
// stream (0x03 may be framed repl.signal payload).

#include "py/runtime.h"
#include "py/mphal.h"
#include "usb_serial_jtag.h"

#if MICROPY_HW_ESP_USB_SERIAL_JTAG

#include "hal/usb_serial_jtag_ll.h"
#include "esp_intr_alloc.h"
#include "esp_private/periph_ctrl.h"
#include "soc/periph_defs.h"
#include "soc/soc_caps.h"
#include "freertos/portmacro.h"

#include "ev_stdio.h"

// EV-MUX congestion counters (debug.info "transport.stats"), indexed by
// ev_control_ingress_t: 0=USJ, 1=CDC, 2=UART.
extern volatile uint32_t ev_transport_rx_ring_full[3];

// Number of bytes in the input buffer, and number of bytes for output chunking.
#define USB_SERIAL_JTAG_PACKET_SZ_BYTES (64)

#if !SOC_RCC_IS_INDEPENDENT
#define MICROPY_USJ_RCC_ATOMIC() PERIPH_RCC_ATOMIC()
#else
#define MICROPY_USJ_RCC_ATOMIC()
#endif

static DRAM_ATTR portMUX_TYPE rx_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t rx_buf[USB_SERIAL_JTAG_PACKET_SZ_BYTES];
static volatile bool terminal_connected = false;

static void usb_serial_jtag_handle_rx(void) {
    if (xPortInIsrContext()) {
        portENTER_CRITICAL_ISR(&rx_mux);
    } else {
        portENTER_CRITICAL(&rx_mux);
    }
    size_t req_len = ringbuf_free(&stdin_ringbuf);
    if (req_len == 0) {
        ++ev_transport_rx_ring_full[0];
    }
    if (req_len > USB_SERIAL_JTAG_PACKET_SZ_BYTES) {
        req_len = USB_SERIAL_JTAG_PACKET_SZ_BYTES;
    }
    size_t len = usb_serial_jtag_ll_read_rxfifo(rx_buf, req_len);
    for (size_t i = 0; i < len; ++i) {
        if ((rx_buf[i] == mp_interrupt_char) && !ev_stdio_mux_enabled_flag) {
            mp_sched_keyboard_interrupt();
        } else {
            ringbuf_put(&stdin_ringbuf, rx_buf[i]);
        }
    }
    if (xPortInIsrContext()) {
        portEXIT_CRITICAL_ISR(&rx_mux);
    } else {
        portEXIT_CRITICAL(&rx_mux);
    }
}

static void usb_serial_jtag_isr_handler(void *arg) {
    uint32_t flags = usb_serial_jtag_ll_get_intsts_mask();
    usb_serial_jtag_ll_clr_intsts_mask(flags);

    if (flags & USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT) {
        usb_serial_jtag_handle_rx();
        mp_hal_wake_main_task_from_isr();
    }

    if (flags & USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY) {
        // As per the ESP-IDF driver, allow for the possibility the USJ just sent a full
        // 64-bit endpoint to the host and now it's waiting for another ZLP to flush the result
        // to the OS
        usb_serial_jtag_ll_txfifo_flush();

        // Disable this interrupt until next time we write into the FIFO
        usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
    }
}

void usb_serial_jtag_init(void) {
    // Do not rely on the ESP-IDF console or CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
    // to leave this peripheral enabled. MICROPY_HW_ESP_USB_SERIAL_JTAG owns
    // the MicroPython interface selection, so explicitly perform the same
    // clock and internal-PHY setup as the ESP-IDF USJ driver.
    MICROPY_USJ_RCC_ATOMIC() {
        usb_serial_jtag_ll_enable_bus_clock(true);
    }
#if USB_SERIAL_JTAG_LL_EXT_PHY_SUPPORTED
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
#else
    usb_serial_jtag_ll_phy_set_defaults();
#endif

    // Note: Don't clear the SERIAL_IN_EMPTY interrupt, as it's possible the
    // bootloader wrote enough data to the host that we need the interrupt to flush it.
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT |
        USB_SERIAL_JTAG_INTR_SOF);
    usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT |
        USB_SERIAL_JTAG_INTR_SOF | USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
    ESP_ERROR_CHECK(esp_intr_alloc(ETS_USB_SERIAL_JTAG_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1,
        usb_serial_jtag_isr_handler, NULL, NULL));
}

void usb_serial_jtag_poll_rx(void) {
    if (usb_serial_jtag_ll_rxfifo_data_available() && ringbuf_free(&stdin_ringbuf)) {
        usb_serial_jtag_handle_rx();
    }
}

void usb_serial_jtag_tx_strn(const char *str, size_t len) {
    (void)usb_serial_jtag_tx_strn_count(str, len);
}

size_t usb_serial_jtag_tx_strn_count(const char *str, size_t len) {
    size_t total = len;
    while (len) {
        size_t l = len;
        if (l > USB_SERIAL_JTAG_PACKET_SZ_BYTES) {
            l = USB_SERIAL_JTAG_PACKET_SZ_BYTES;
        }
        TickType_t start_tick = xTaskGetTickCount();
        while (!usb_serial_jtag_ll_txfifo_writable()) {
            TickType_t now_tick = xTaskGetTickCount();
            if (!terminal_connected || now_tick > (start_tick + pdMS_TO_TICKS(200))) {
                terminal_connected = false;
                return total - len;
            }
        }
        terminal_connected = true;
        l = usb_serial_jtag_ll_write_txfifo((const uint8_t *)str, l);
        str += l;
        len -= l;
        usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
    }
    usb_serial_jtag_ll_txfifo_flush();
    return total;
}

#endif // MICROPY_HW_ESP_USB_SERIAL_JTAG

/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Ibrahim Abdelkader <iabdalkader@openmv.io>
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

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/stream.h"
#include "extmod/modmachine.h"

#include "mp_usbd.h"
#include "mp_usbd_cdc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ev_stdio.h"
#include "ev_cdc_rx.h"
#include "usb_auto_download.h"

// EV-MUX congestion counters (debug.info "transport.stats"), indexed by
// ev_control_ingress_t: 0=USJ, 1=CDC, 2=UART.
extern volatile uint32_t ev_transport_rx_ring_full[3];

extern ringbuf_t cdc_stdin_ringbuf;

#if MICROPY_HW_USB_CDC && MICROPY_HW_ENABLE_USBDEV && !MICROPY_EXCLUDE_SHARED_TINYUSB_USBD_CDC

static uint8_t cdc_itf_pending; // keep track of cdc interfaces which need attention to poll
static int8_t cdc_connected_flush_delay = 0;
static uint32_t cdc_rx_generation_tags[EV_CDC_RX_RING_CAPACITY];
static ev_cdc_rx_state_t cdc_rx_state = {
    .generation = 1,
    .mounted = false,
    .mount_seen = false,
    .dtr = false,
    .valid = true,
};

static void mp_usbd_cdc_transport_ring_clear(void) {
    cdc_stdin_ringbuf.iget = cdc_stdin_ringbuf.iput = 0;
}

static void mp_usbd_cdc_transport_flush_fifo(void *context) {
    uint8_t itf = (uint8_t)(uintptr_t)context;
    if (itf < 8) {
        cdc_itf_pending &= (uint8_t)~(1u << itf);
        // TinyUSB implements this as a constant-time FIFO clear and OUT
        // endpoint rearm. It is safe here because all callbacks run under the
        // serialized TinyUSB task lock.
        if (tusb_inited()) {
            tud_cdc_n_read_flush(itf);
        }
    }
}

static void mp_usbd_cdc_transport_flush_rx(void *context) {
    mp_usbd_cdc_transport_ring_clear();
    mp_usbd_cdc_transport_flush_fifo(context);
}

static void mp_usbd_cdc_transport_sync_bus(bool mounted) {
    (void)ev_cdc_rx_bus_state(&cdc_rx_state, mounted,
                              mp_usbd_cdc_transport_flush_rx, (void *)(uintptr_t)0);
}

static bool mp_usbd_cdc_transport_rx_storage_valid(void) {
    if (cdc_stdin_ringbuf.size != EV_CDC_RX_RING_CAPACITY) {
        cdc_rx_state.valid = false;
        return false;
    }
    return cdc_rx_state.valid;
}

// TinyUSB invokes these callbacks from its serialized task. A successful
// configuration mount is a new physical RX generation even when a bus reset
// skipped tud_umount_cb(); it runs before the host can send the next bulk packet.
void tud_mount_cb(void) {
    (void)ev_cdc_rx_bus_mount(&cdc_rx_state,
                              mp_usbd_cdc_transport_flush_rx, (void *)(uintptr_t)0);
}

void tud_umount_cb(void) {
    (void)ev_cdc_rx_bus_unmount(&cdc_rx_state,
                                mp_usbd_cdc_transport_flush_rx, (void *)(uintptr_t)0);
}

void mp_usbd_cdc_transport_endpoint_state(bool *present,
                                          bool *mounted,
                                          bool *ready) {
    mp_usbd_lock();
    bool initialized = tusb_inited();
    if (present != NULL) {
        *present = initialized && tud_connected();
    }
    if (mounted != NULL) {
        *mounted = initialized && tud_mounted();
    }
    if (ready != NULL) {
        *ready = initialized && tud_cdc_ready();
    }
    mp_usbd_unlock();
}

uintptr_t mp_usbd_cdc_poll_interfaces(uintptr_t poll_flags) {
    uintptr_t ret = 0;
    mp_usbd_lock();
    if (!cdc_itf_pending) {
        // Explicitly run the USB stack as the scheduler may be locked (eg we are in
        // an interrupt handler) while there is data pending.
        mp_usbd_task();
    }

    // any CDC interfaces left to poll?
    if (cdc_itf_pending && ringbuf_free(&cdc_stdin_ringbuf)) {
        for (uint8_t itf = 0; itf < 8; ++itf) {
            if (cdc_itf_pending & (1 << itf)) {
                tud_cdc_rx_cb(itf);
                if (!cdc_itf_pending) {
                    break;
                }
            }
        }
    }
    mp_usbd_unlock();
    if ((poll_flags & MP_STREAM_POLL_RD) && ringbuf_peek(&cdc_stdin_ringbuf) != -1) {
        ret |= MP_STREAM_POLL_RD;
    }
    if ((poll_flags & MP_STREAM_POLL_WR) &&
        (!tud_cdc_connected() || (tud_cdc_connected() && tud_cdc_write_available() > 0))) {
        // Always allow write when not connected, fifo will retain latest.
        // When connected operate as blocking, only allow if space is available.
        ret |= MP_STREAM_POLL_WR;
    }
    return ret;
}

// Pump the USB device stack from the EV transport task (see
// platform/ev_control_transport.c). Keeps CDC RX flowing and services
// interfaces whose RX stalled on a full ring buffer, independently of what
// the MicroPython main task is doing.
void mp_usbd_cdc_transport_pump(void) {
    if (!tusb_inited()) {
        return;
    }
    mp_usbd_lock();
    mp_usbd_task();
    if (cdc_itf_pending && ringbuf_free(&cdc_stdin_ringbuf)) {
        for (uint8_t itf = 0; itf < 8; ++itf) {
            if (cdc_itf_pending & (1 << itf)) {
                tud_cdc_rx_cb(itf);
                if (!cdc_itf_pending) {
                    break;
                }
            }
        }
    }
    mp_usbd_unlock();
}

// Compatibility helper for callers that need a full reset. Link open must use
// generation filtering instead so a first packet received in the same USB pump
// is retained.
void mp_usbd_cdc_transport_clear_rx(void) {
    mp_usbd_lock();
    mp_usbd_cdc_transport_ring_clear();
    mp_usbd_cdc_transport_flush_fifo((void *)(uintptr_t)0);
    mp_usbd_unlock();
}

void mp_usbd_cdc_transport_rx_state(bool filter_generation,
                                    bool *mounted_out,
                                    uint32_t *generation,
                                    bool *generation_valid) {
    mp_usbd_lock();
    bool mounted = tusb_inited() && tud_mounted();
    mp_usbd_cdc_transport_sync_bus(mounted);
    bool valid = mp_usbd_cdc_transport_rx_storage_valid();
    if (filter_generation && mounted && valid) {
        (void)ev_cdc_rx_ring_filter(cdc_stdin_ringbuf.buf,
                                    cdc_rx_generation_tags,
                                    cdc_stdin_ringbuf.size,
                                    &cdc_stdin_ringbuf.iget,
                                    &cdc_stdin_ringbuf.iput,
                                    true,
                                    cdc_rx_state.generation);
    } else if (!mounted || !valid) {
        mp_usbd_cdc_transport_ring_clear();
    }
    if (mounted_out != NULL) {
        *mounted_out = mounted && valid;
    }
    if (generation != NULL) {
        *generation = cdc_rx_state.generation;
    }
    if (generation_valid != NULL) {
        *generation_valid = valid;
    }
    mp_usbd_unlock();
}

size_t mp_usbd_cdc_transport_rx_read(uint32_t expected_generation,
                                     void *buffer,
                                     size_t capacity) {
    if ((buffer == NULL) || (capacity == 0)) {
        return 0;
    }
    mp_usbd_lock();
    bool mounted = tusb_inited() && tud_mounted();
    mp_usbd_cdc_transport_sync_bus(mounted);
    bool valid = mp_usbd_cdc_transport_rx_storage_valid();
    if (!valid || !mounted) {
        mp_usbd_cdc_transport_ring_clear();
        mp_usbd_unlock();
        return 0;
    }

    (void)ev_cdc_rx_ring_filter(cdc_stdin_ringbuf.buf,
                                cdc_rx_generation_tags,
                                cdc_stdin_ringbuf.size,
                                &cdc_stdin_ringbuf.iget,
                                &cdc_stdin_ringbuf.iput,
                                true,
                                cdc_rx_state.generation);
    if (cdc_rx_state.generation != expected_generation) {
        mp_usbd_unlock();
        return 0;
    }

    size_t length = 0;
    uint8_t *bytes = (uint8_t *)buffer;
    while ((length < capacity) && (cdc_stdin_ringbuf.iget != cdc_stdin_ringbuf.iput)) {
        bytes[length++] = (uint8_t)ringbuf_get(&cdc_stdin_ringbuf);
    }
    mp_usbd_unlock();
    return length;
}

bool mp_usbd_cdc_transport_rx_restart(uint32_t *generation_out) {
    mp_usbd_lock();
    bool mounted = tusb_inited() && tud_mounted();
    mp_usbd_cdc_transport_sync_bus(mounted);
    bool valid = mounted && mp_usbd_cdc_transport_rx_storage_valid() &&
                 ev_cdc_rx_restart(&cdc_rx_state,
                                   mp_usbd_cdc_transport_flush_rx,
                                   (void *)(uintptr_t)0);
    if (valid) {
        if (generation_out != NULL) {
            *generation_out = cdc_rx_state.generation;
        }
    }
    mp_usbd_unlock();
    return valid;
}

void mp_usbd_cdc_transport_clear_rx_if_unmounted(void) {
    mp_usbd_lock();
    bool mounted = tusb_inited() && tud_mounted();
    mp_usbd_cdc_transport_sync_bus(mounted);
    if (!mounted || !mp_usbd_cdc_transport_rx_storage_valid()) {
        mp_usbd_cdc_transport_flush_rx((void *)(uintptr_t)0);
    }
    mp_usbd_unlock();
}

void MICROPY_WRAP_TUD_CDC_RX_CB(tud_cdc_rx_cb)(uint8_t itf) {
    // consume pending USB data immediately to free usb buffer and keep the endpoint from stalling.
    // in case the ringbuffer is full, mark the CDC interface that need attention later on for polling
    bool mounted = tud_mounted();
    mp_usbd_cdc_transport_sync_bus(mounted);
    cdc_itf_pending &= ~(1 << itf);
    bool storage_valid = mp_usbd_cdc_transport_rx_storage_valid();
    for (uint32_t bytes_avail = tud_cdc_n_available(itf); bytes_avail > 0; --bytes_avail) {
        if (!mounted || !storage_valid) {
            (void)tud_cdc_read_char();
        } else if (ringbuf_free(&cdc_stdin_ringbuf)) {
            int data_char = tud_cdc_read_char();
            #if MICROPY_KBD_EXCEPTION
            // When EV-MUX owns the byte stream, 0x03 may be framed payload
            // (repl.signal); pass it through and let the parser deliver it.
            if ((data_char == mp_interrupt_char) && !ev_stdio_mux_enabled_flag) {
                // Clear only the CDC input buffer.
                mp_usbd_cdc_transport_ring_clear();
                // and stop
                mp_sched_keyboard_interrupt();
            } else {
                (void)ev_cdc_rx_ring_put(cdc_stdin_ringbuf.buf,
                                         cdc_rx_generation_tags,
                                         cdc_stdin_ringbuf.size,
                                         &cdc_stdin_ringbuf.iget,
                                         &cdc_stdin_ringbuf.iput,
                                         (uint8_t)data_char,
                                         cdc_rx_state.generation);
            }
            #else
            (void)ev_cdc_rx_ring_put(cdc_stdin_ringbuf.buf,
                                     cdc_rx_generation_tags,
                                     cdc_stdin_ringbuf.size,
                                     &cdc_stdin_ringbuf.iget,
                                     &cdc_stdin_ringbuf.iput,
                                     (uint8_t)data_char,
                                     cdc_rx_state.generation);
            #endif
        } else {
            ++ev_transport_rx_ring_full[1];
            cdc_itf_pending |= (1 << itf);
            return;
        }
    }
}

mp_uint_t mp_usbd_cdc_tx_strn(const char *str, mp_uint_t len) {
    if (!tusb_inited()) {
        return 0;
    }
    mp_uint_t last_write = mp_hal_ticks_ms();
    size_t i = 0;
    while (i < len) {
        uint32_t n = len - i;

        if (tud_cdc_connected()) {
            // Limit write to available space in tx buffer when connected.
            //
            // (If not connected then we write everything to the fifo, expecting
            // it to overwrite old data so it will have latest data buffered
            // when host connects.)
            n = MIN(n, tud_cdc_write_available());
        }

        uint32_t n2 = tud_cdc_write(str + i, n);
        tud_cdc_write_flush();
        i += n2;

        if (i < len) {
            if (n2 > 0) {
                // reset the timeout each time we successfully write to the FIFO
                last_write = mp_hal_ticks_ms();
            } else {
                if ((mp_uint_t)(mp_hal_ticks_ms() - last_write) >= MICROPY_HW_USB_CDC_TX_TIMEOUT) {
                    break; // Timeout
                }

                if (tud_cdc_connected()) {
                    // If we know we're connected then we can wait for host to make
                    // more space
                    mp_event_wait_ms(1);
                }
            }

            // Always explicitly run the USB stack as the scheduler may be
            // locked (eg we are in an interrupt handler), while there is data
            // or a state change pending.
            mp_usbd_task();
        }
    }
    return i;
}

// Task-safe variant of mp_usbd_cdc_tx_strn: identical logic, but waits with
// vTaskDelay instead of mp_event_wait_ms. Never touches MicroPython
// scheduler/thread state, so it is safe to call from any FreeRTOS task
// (the EV transport task writes EV-MUX frames to CDC from there).
mp_uint_t mp_usbd_cdc_tx_strn_anytask(const char *str, mp_uint_t len) {
    if (!tusb_inited()) {
        return 0;
    }
    mp_uint_t last_write = mp_hal_ticks_ms();
    size_t i = 0;
    while (i < len) {
        uint32_t n = len - i;

        if (tud_cdc_connected()) {
            n = MIN(n, tud_cdc_write_available());
        }

        uint32_t n2 = tud_cdc_write(str + i, n);
        tud_cdc_write_flush();
        i += n2;

        if (i < len) {
            if (n2 > 0) {
                // reset the timeout each time we successfully write to the FIFO
                last_write = mp_hal_ticks_ms();
            } else {
                if ((mp_uint_t)(mp_hal_ticks_ms() - last_write) >= MICROPY_HW_USB_CDC_TX_TIMEOUT) {
                    break; // Timeout
                }

                if (tud_cdc_connected()) {
                    // Wait for the host to make more space.
                    vTaskDelay(1);
                }
            }

            // Always explicitly run the USB stack as the scheduler may be
            // locked (eg we are in an interrupt handler), while there is data
            // or a state change pending.
            mp_usbd_task();
        }
    }
    return i;
}

// Single nonblocking CDC FIFO write for the EV-MUX endpoint scheduler. This
// avoids the task-safe write-all helper's timeout loop monopolizing the TX
// worker when a host stops reading.
size_t mp_usbd_cdc_tx_some_anytask(const char *str, size_t len) {
    if ((str == NULL) || (len == 0)) {
        return 0;
    }
    mp_usbd_lock();
    if (!tusb_inited() || !tud_cdc_ready()) {
        mp_usbd_unlock();
        return 0;
    }
    size_t available = tud_cdc_write_available();
    if (len > available) {
        len = available;
    }
    size_t written = (len != 0) ? tud_cdc_write(str, (uint32_t)len) : 0;
    if (written != 0) {
        tud_cdc_write_flush();
    }
    mp_usbd_unlock();
    return written;
}

void MICROPY_WRAP_TUD_SOF_CB(tud_sof_cb)(uint32_t frame_count) {
    if (--cdc_connected_flush_delay < 0) {
        // Finished on-connection delay, disable SOF interrupt again.
        tud_sof_cb_enable(false);
        tud_cdc_write_flush();
    }
}

#endif

#if MICROPY_HW_ENABLE_USBDEV && ( \
    MICROPY_HW_USB_CDC_1200BPS_TOUCH || \
    MICROPY_HW_USB_CDC || \
    MICROPY_HW_USB_CDC_DTR_RTS_BOOTLOADER)

#if MICROPY_HW_USB_CDC_1200BPS_TOUCH
static mp_sched_node_t mp_bootloader_sched_node;

static void usbd_cdc_run_bootloader_task(mp_sched_node_t *node) {
    mp_hal_delay_ms(250);
    machine_bootloader(0, NULL);
}
#endif

void MICROPY_WRAP_TUD_CDC_LINE_STATE_CB(tud_cdc_line_state_cb)(uint8_t itf, bool dtr, bool rts) {
    #if MICROPY_HW_USB_CDC && !MICROPY_EXCLUDE_SHARED_TINYUSB_USBD_CDC
    if ((itf == 0) && ev_cdc_rx_dtr_update(&cdc_rx_state, dtr) &&
        !ev_stdio_mux_enabled()) {
        // Preserve raw-REPL close behavior. DTR does not change framed RX
        // generation, link readiness, or session ownership.
        mp_usbd_cdc_transport_ring_clear();
        mp_usbd_cdc_transport_flush_fifo((void *)(uintptr_t)itf);
    }
    #endif
    #if MICROPY_HW_USB_CDC && !MICROPY_EXCLUDE_SHARED_TINYUSB_USBD_CDC
    if (dtr) {
        // A host application has started to open the cdc serial port.
        // Drop stale bytes retained in the tx fifo from a previous (possibly
        // torn) session — but only when EV-MUX is off. With the mux on, sink
        // writes are valid without DTR, so clearing here could erase queued
        // protocol replies or events.
        if (!ev_stdio_mux_enabled()) {
            tud_cdc_write_clear();
        }
        // Wait a few ms for host to be ready then send tx buffer.
        // High speed connection SOF fires at 125us, full speed at 1ms.
        cdc_connected_flush_delay = (tud_speed_get() == TUSB_SPEED_HIGH) ? 128 : 16;
        tud_sof_cb_enable(true);
    }
    #endif
    #if MICROPY_HW_USB_CDC_DTR_RTS_BOOTLOADER
    esp_vision_usb_auto_download_line_state(dtr, rts);
    #endif
    #if MICROPY_HW_USB_CDC_1200BPS_TOUCH
    if (dtr == false && rts == false) {
        // Device is disconnected.
        cdc_line_coding_t line_coding;
        tud_cdc_n_get_line_coding(itf, &line_coding);
        if (line_coding.bit_rate == 1200) {
            // Delay bootloader jump to allow the USB stack to service endpoints.
            mp_sched_schedule_node(&mp_bootloader_sched_node, usbd_cdc_run_bootloader_task);
        }
    }
    #endif
}

#endif

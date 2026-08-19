/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb_auto_download.h"

#include <stdatomic.h>

#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "usb.h"

#define USB_RESTART_FALLBACK_US (500 * 1000)
#define USB_RESTART_DEFER_US (50 * 1000)

#if CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"
#include "esp32s3/rom/usb/usb_dc.h"
#include "esp32s3/rom/usb/usb_persist.h"
#endif

#ifndef MICROPY_HW_USB_AUTO_DOWNLOAD_TEST_LOG
#define MICROPY_HW_USB_AUTO_DOWNLOAD_TEST_LOG (0)
#endif

#if MICROPY_HW_USB_AUTO_DOWNLOAD_TEST_LOG
#define USB_AUTO_DOWNLOAD_LOG(format, ...) esp_rom_printf("[usb-auto-download] " format "\r\n", ##__VA_ARGS__)
#else
#define USB_AUTO_DOWNLOAD_LOG(format, ...) do { } while (0)
#endif

typedef enum {
    USB_REBOOT_NONE,
    USB_REBOOT_NORMAL,
    USB_REBOOT_BOOTLOADER,
} usb_reboot_mode_t;

static atomic_int s_reboot_mode = ATOMIC_VAR_INIT(USB_REBOOT_NONE);
static atomic_bool s_download_mode = ATOMIC_VAR_INIT(false);
static esp_timer_handle_t s_restart_timer;

static void usb_auto_download_before_restart(void)
{
    const usb_reboot_mode_t mode = atomic_load_explicit(&s_reboot_mode, memory_order_acquire);
    USB_AUTO_DOWNLOAD_LOG("shutdown mode=%d", mode);
    if (mode == USB_REBOOT_BOOTLOADER) {
        REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    } else if (mode == USB_REBOOT_NORMAL) {
        REG_CLR_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    }
}

static void usb_auto_download_restart(void *arg)
{
    (void)arg;
    USB_AUTO_DOWNLOAD_LOG("restart timer fired");
    if (atomic_load_explicit(&s_reboot_mode, memory_order_acquire) == USB_REBOOT_BOOTLOADER) {
        /*
         * Keep this hand-off independent from MicroPython's machine module,
         * but follow the same ROM persistence contract used by
         * machine_bootloader_rtc(). The persistence preparation makes the
         * running TinyUSB CDC session look busy while the ROM takes over.
         */
#if CONFIG_IDF_TARGET_ESP32S3
        usb_usj_mode();
        int persist_ret = usb_dc_prepare_persist();
        if (persist_ret != 0) {
            USB_AUTO_DOWNLOAD_LOG("usb persistence prepare failed=%d", persist_ret);
        }
        chip_usb_set_persist_flags(USBDC_BOOT_DFU);
#endif
    }
    esp_restart();
}

esp_err_t esp_vision_usb_auto_download_init(void)
{
    if (s_restart_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = usb_auto_download_restart,
        .name = "usb_reboot",
    };
    esp_err_t ret = esp_timer_create(&timer_args, &s_restart_timer);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_register_shutdown_handler(usb_auto_download_before_restart);
    if (ret != ESP_OK) {
        (void)esp_timer_delete(s_restart_timer);
        s_restart_timer = NULL;
    }
    return ret;
}

void esp_vision_usb_auto_download_line_state(bool dtr, bool rts)
{
    USB_AUTO_DOWNLOAD_LOG("line state RTS=%d DTR=%d", rts, dtr);

    /*
     * Match the USB-Serial/JTAG CDC state table used by esptool:
     *
     *   RTS DTR  action
     *    0   0   clear download-mode latch
     *    0   1   set download-mode latch
     *    1   0   reset using the latched mode
     *    1   1   no action
    */
    if (!rts) {
        atomic_store_explicit(&s_download_mode, dtr, memory_order_release);
        if (!dtr && atomic_load_explicit(&s_reboot_mode, memory_order_acquire) != USB_REBOOT_NONE) {
            /*
             * This is the final idle state in esptool's USB-Serial/JTAG
             * sequence. Do not tear down TinyUSB from the line-state callback:
             * the callback is still completing the host's control request.
             * Defer the persistence/PHY hand-off to the timer task.
             */
            if (s_restart_timer != NULL) {
                (void)esp_timer_stop(s_restart_timer);
                if (esp_timer_start_once(s_restart_timer, USB_RESTART_DEFER_US) == ESP_OK) {
                    return;
                }
            }
            /* Keep a reset path if timer setup failed; this is the last-resort
             * fallback for an incomplete board initialization. */
            USB_AUTO_DOWNLOAD_LOG("deferred restart unavailable; restarting now");
            usb_auto_download_restart(NULL);
        }
        return;
    }
    if (dtr) {
        return;
    }

    const usb_reboot_mode_t requested_mode =
        atomic_load_explicit(&s_download_mode, memory_order_acquire) ? USB_REBOOT_BOOTLOADER : USB_REBOOT_NORMAL;
    int expected_mode = USB_REBOOT_NONE;
    if (!atomic_compare_exchange_strong_explicit(&s_reboot_mode, &expected_mode, requested_mode,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        return;
    }

    /* Fall back if a non-standard host omits esptool's final idle state. */
    if (s_restart_timer == NULL || esp_timer_start_once(s_restart_timer, USB_RESTART_FALLBACK_US) != ESP_OK) {
        atomic_store_explicit(&s_reboot_mode, USB_REBOOT_NONE, memory_order_release);
    }
}

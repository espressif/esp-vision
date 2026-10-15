/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define CONFIG_IDF_TARGET_ESP32P4 (1)
#include "soc/lp_system_reg.h"
#include "esp_timer.h"
#include "usb_auto_download.h"

#include "../../platform/usb_auto_download.c"

struct esp_timer {
    esp_timer_cb_t callback;
    bool active;
    uint64_t timeout_us;
};

static struct esp_timer s_timer;
static shutdown_handler_t s_shutdown_handler;
static uint32_t s_lp_system_reg;
static uint32_t s_restart_count;
static bool s_machine_restart;
static jmp_buf s_machine_restart_jump;

#define MP_NORETURN __attribute__((noreturn))
#include "machine_bootloader_test.inc"

void test_reg_set_bit(uint32_t reg, uint32_t mask)
{
    assert(reg == LP_SYSTEM_REG_SYS_CTRL_REG);
    s_lp_system_reg |= mask;
}

void test_reg_clear_bit(uint32_t reg, uint32_t mask)
{
    assert(reg == LP_SYSTEM_REG_SYS_CTRL_REG);
    s_lp_system_reg &= ~mask;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out_timer)
{
    assert(args != NULL);
    assert(args->callback != NULL);
    s_timer.callback = args->callback;
    *out_timer = &s_timer;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    assert(timer == &s_timer);
    s_timer.callback = NULL;
    s_timer.active = false;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    assert(timer == &s_timer);
    s_timer.timeout_us = timeout_us;
    s_timer.active = true;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    assert(timer == &s_timer);
    s_timer.active = false;
    return ESP_OK;
}

esp_err_t esp_register_shutdown_handler(shutdown_handler_t handler)
{
    assert(handler != NULL);
    s_shutdown_handler = handler;
    return ESP_OK;
}

void esp_restart(void)
{
    ++s_restart_count;
    if (s_shutdown_handler != NULL) {
        s_shutdown_handler();
    }
    if (s_machine_restart) {
        longjmp(s_machine_restart_jump, 1);
    }
}

static void fire_timer(void)
{
    assert(s_timer.active);
    assert(s_timer.callback != NULL);
    s_timer.active = false;
    s_timer.callback(NULL);
}

static void test_bootloader_request(void)
{
    const uint32_t unrelated_control_bit = 1U << 7;
    s_lp_system_reg = unrelated_control_bit;

    assert(esp_vision_usb_auto_download_init() == ESP_OK);
    esp_vision_usb_auto_download_line_state(true, false);
    esp_vision_usb_auto_download_line_state(false, true);
    assert(s_restart_count == 0);
    assert(s_timer.active);
    assert(s_timer.timeout_us == 500000U);

    /* esptool's final idle state must defer the reset, not tear CDC down inline. */
    esp_vision_usb_auto_download_line_state(false, false);
    assert(s_restart_count == 0);
    assert(s_timer.active);
    assert(s_timer.timeout_us == 50000U);

    fire_timer();
    assert(s_restart_count == 1);
    assert((s_lp_system_reg & LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT) != 0);
    assert((s_lp_system_reg & unrelated_control_bit) != 0);
}

static void test_normal_reset_request(void)
{
    const uint32_t unrelated_control_bit = 1U << 7;
    s_lp_system_reg = unrelated_control_bit | LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT;

    assert(esp_vision_usb_auto_download_init() == ESP_OK);
    esp_vision_usb_auto_download_line_state(false, false);
    esp_vision_usb_auto_download_line_state(false, true);
    assert(s_restart_count == 0);
    assert(s_timer.active);
    assert(s_timer.timeout_us == 500000U);

    esp_vision_usb_auto_download_line_state(false, false);
    assert(s_restart_count == 0);
    assert(s_timer.active);
    assert(s_timer.timeout_us == 50000U);

    fire_timer();
    assert(s_restart_count == 1);
    assert((s_lp_system_reg & LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT) == 0);
    assert((s_lp_system_reg & unrelated_control_bit) != 0);
}

static void test_dtr_only_open_close(void)
{
    s_lp_system_reg = 0;
    assert(esp_vision_usb_auto_download_init() == ESP_OK);

    /* DTR-only open and close with RTS low never request a reset. */
    esp_vision_usb_auto_download_line_state(true, false);
    esp_vision_usb_auto_download_line_state(false, false);
    assert(s_restart_count == 0);
    assert(!s_timer.active);
    assert(s_lp_system_reg == 0);
}

static void test_machine_bootloader(bool initialize_usb)
{
    if (initialize_usb) {
        assert(esp_vision_usb_auto_download_init() == ESP_OK);
        // An already deferred normal USB reset must not override Python's request.
        esp_vision_usb_auto_download_line_state(false, false);
        esp_vision_usb_auto_download_line_state(false, true);
    }
    s_machine_restart = true;
    const uint32_t initial[] = {0x1dfbc7f8U, 0x1fffc7f8U, 0U, 0x1dfbc7fcU};
    for (unsigned i = 0; i < sizeof(initial) / sizeof(initial[0]); ++i) {
        s_lp_system_reg = initial[i];
        if (setjmp(s_machine_restart_jump) == 0) {
            machine_bootloader_rtc();
        }
        assert(s_lp_system_reg == (initial[i] | LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT));
        assert(s_restart_count == i + 1);
        assert(!s_timer.active);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "boot") == 0) {
        test_bootloader_request();
    } else if (strcmp(argv[1], "normal") == 0) {
        test_normal_reset_request();
    } else if (strcmp(argv[1], "dtr") == 0) {
        test_dtr_only_open_close();
    } else if (strcmp(argv[1], "machine") == 0) {
        test_machine_bootloader(true);
    } else if (strcmp(argv[1], "machine-no-init") == 0) {
        test_machine_bootloader(false);
    } else {
        assert(false);
    }
    return 0;
}

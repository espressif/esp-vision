/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "soc/lp_system_reg.h"
#include "esp_timer.h"
#include "usb_auto_download.h"

#include "../../platform/usb_auto_download.c"

struct esp_timer {
    esp_timer_cb_t callback;
    bool active;
    uint64_t timeout_us;
    uint64_t deadline_us;
};

static struct esp_timer s_timer;
static shutdown_handler_t s_shutdown_handlers[5];
static uint32_t s_register_count;
static uint32_t s_lp_system_reg;
static uint32_t s_restart_count;
static uint32_t s_cpu_reset_count;
static uint32_t s_system_reset_count;
static bool s_rom_reset_returns;
static bool s_restart_jump_active;
static jmp_buf s_restart_jump;
static unsigned s_cleanup_order[3];
static unsigned s_cleanup_count;
static uint64_t s_now_us;
static bool s_host_dtr;
static bool s_host_rts;
static bool s_driver_dtr;
static bool s_driver_rts;
static bool s_coalesce_controls;
static unsigned s_control_writes;
static unsigned s_control_callbacks;

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
    s_timer.deadline_us = s_now_us + timeout_us;
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
    ++s_register_count;
    for (unsigned i = 0; i < 5; ++i) {
        if (s_shutdown_handlers[i] == handler) {
            return ESP_ERR_INVALID_STATE;
        }
        if (s_shutdown_handlers[i] == NULL) {
            s_shutdown_handlers[i] = handler;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

void esp_restart(void)
{
    ++s_restart_count;
    // Match IDF's actual reverse registration order, not a single hook stub.
    for (int i = 4; i >= 0; --i) {
        if (s_shutdown_handlers[i] != NULL) {
            s_shutdown_handlers[i]();
        }
    }
    ++s_cpu_reset_count;
    assert(s_restart_jump_active);
    longjmp(s_restart_jump, 1);
}

void esp_rom_software_reset_system(void)
{
    assert(s_restart_count != 0);
    ++s_system_reset_count;
    if (s_cleanup_count != 0) {
        assert(s_cleanup_count == 2);
        assert(s_cleanup_order[0] == 2);
        assert(s_cleanup_order[1] == 1);
        s_cleanup_order[s_cleanup_count++] = 3;
        assert((s_lp_system_reg & LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT) != 0);
    }
    if (s_rom_reset_returns) {
        puts("rom-reset-returned");
        fflush(stdout);
        return;
    }
    assert(s_restart_jump_active);
    longjmp(s_restart_jump, 1);
}

static void run_restart(void (*operation)(void))
{
    s_restart_jump_active = true;
    if (setjmp(s_restart_jump) == 0) {
        operation();
        assert(false);
    }
    s_restart_jump_active = false;
}

static void initialize_startup(void)
{
#if CONFIG_IDF_TARGET_ESP32S31
    // Also runs on USJ-only Chatbot; no CDC timer is required.
    assert(esp_vision_restart_init() == ESP_OK);
#endif
}

static void assert_reset_path(uint32_t count, bool download)
{
#if CONFIG_IDF_TARGET_ESP32S31
    assert(s_system_reset_count == (download ? count : 0));
    assert(s_cpu_reset_count == (download ? 0 : count));
#else
    (void)download;
    assert(s_system_reset_count == 0);
    assert(s_cpu_reset_count == count);
#endif
}

static void fire_timer(void)
{
    assert(s_timer.active);
    assert(s_timer.callback != NULL);
    s_timer.active = false;
    s_timer.callback(NULL);
}

static void advance_time_us(uint64_t elapsed)
{
    const uint64_t until = s_now_us + elapsed;
    while (s_timer.active && s_timer.deadline_us <= until) {
        s_now_us = s_timer.deadline_us;
        run_restart(fire_timer);
    }
    s_now_us = until;
}

static void deliver_control(bool dtr, bool rts)
{
    ++s_control_writes;
    if (s_coalesce_controls && s_driver_dtr == dtr && s_driver_rts == rts) {
        return;
    }
    s_driver_dtr = dtr;
    s_driver_rts = rts;
    ++s_control_callbacks;
    esp_vision_usb_auto_download_line_state(dtr, rts);
}

static void host_set_dtr(bool value)
{
    s_host_dtr = value;
    // A single-pin update retains the driver's other actual pin, which may
    // differ from pyserial's cached pre-open setting until it is applied.
    deliver_control(value, s_driver_rts);
}

static void host_set_rts(bool value)
{
    s_host_rts = value;
    deliver_control(s_driver_dtr, value);
}

static void model_linux_open(bool dtr, bool rts)
{
    s_host_dtr = dtr;
    s_host_rts = rts;
    // Model cdc-acm's asserted open followed by pyserial's DTR-then-RTS
    // application. These are source-derived inputs, not measured wire states.
    deliver_control(true, true);
    host_set_dtr(s_host_dtr);
    host_set_rts(s_host_rts);
}

static void test_complete_control_sequence(bool coalesce)
{
    initialize_startup();
    assert(esp_vision_usb_auto_download_init() == ESP_OK);
    s_lp_system_reg = 1U << 7;
    s_coalesce_controls = coalesce;
    model_linux_open(true, false); // Core's running-application serial policy.
    assert(!s_timer.active);
    assert(s_restart_count == 0);

    const struct {
        char signal;
        bool value;
        uint32_t wait_us;
    } controls[] = {
        // All host calls from the official USB-JTAG reset trace, including
        // repeated DTR writes used to propagate RTS changes on Windows.
        {'R', false, 0}, {'D', true, 0}, {'D', false, 100000},
        {'D', true, 0}, {'R', false, 0}, {'D', true, 100000},
        {'R', true, 0}, {'D', true, 0}, {'D', false, 0},
        {'R', true, 0}, {'D', false, 100000},
        {'D', false, 0}, {'R', false, 0}, {'D', false, 0},
    };
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        if (controls[i].signal == 'D') {
            host_set_dtr(controls[i].value);
        } else {
            host_set_rts(controls[i].value);
        }
        advance_time_us(controls[i].wait_us);
        assert(s_restart_count == 0);
    }

    assert(s_control_writes == 17);
    assert(s_control_callbacks == (coalesce ? 7U : 17U));
    assert(s_now_us == 300000);
    assert(s_timer.active);
    assert(s_timer.timeout_us == 50000);
    assert(s_timer.deadline_us == 350000);
    advance_time_us(49999);
    assert(s_restart_count == 0);
    advance_time_us(1);
    assert(s_restart_count == 1);
    assert_reset_path(1, true);
    assert(s_lp_system_reg == ((1U << 7) | LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT));
}

static void test_false_false_open(void)
{
    initialize_startup();
    assert(esp_vision_usb_auto_download_init() == ESP_OK);
    s_lp_system_reg = (1U << 7) | LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT;
    s_coalesce_controls = true;
    model_linux_open(false, false);
    // 11 -> 01 latches NORMAL; 00 defers it by 50 ms. Cached False/False
    // before open therefore does not model a passive opening on Linux.
    assert(s_restart_count == 0);
    assert(s_timer.active);
    assert(s_timer.timeout_us == 50000);
    advance_time_us(49999);
    assert(s_restart_count == 0);
    advance_time_us(1);
    assert(s_restart_count == 1);
    assert_reset_path(1, false);
    assert(s_lp_system_reg == (1U << 7));
}

static void test_bootloader_request(void)
{
    const uint32_t unrelated_control_bit = 1U << 7;
    s_lp_system_reg = unrelated_control_bit;

    initialize_startup();
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

    run_restart(fire_timer);
    assert(s_restart_count == 1);
    assert_reset_path(1, true);
    assert((s_lp_system_reg & LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT) != 0);
    assert((s_lp_system_reg & unrelated_control_bit) != 0);
}

static void test_normal_reset_request(void)
{
    const uint32_t unrelated_control_bit = 1U << 7;
    s_lp_system_reg = unrelated_control_bit | LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT;

    initialize_startup();
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

    run_restart(fire_timer);
    assert(s_restart_count == 1);
    assert_reset_path(1, false);
    assert((s_lp_system_reg & LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT) == 0);
    assert((s_lp_system_reg & unrelated_control_bit) != 0);
}

static void test_dtr_only_open_close(void)
{
    s_lp_system_reg = 0;
    initialize_startup();
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
    initialize_startup();
    if (initialize_usb) {
        assert(esp_vision_usb_auto_download_init() == ESP_OK);
        // An already deferred normal USB reset must not override Python's request.
        esp_vision_usb_auto_download_line_state(false, false);
        esp_vision_usb_auto_download_line_state(false, true);
    }
    const uint32_t initial[] = {0x1dfbc7f8U, 0x1fffc7f8U, 0U, 0x1dfbc7fcU};
    for (unsigned i = 0; i < sizeof(initial) / sizeof(initial[0]); ++i) {
        s_lp_system_reg = initial[i];
        run_restart(machine_bootloader_rtc);
        assert(s_lp_system_reg == (initial[i] | LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT));
        assert(s_restart_count == i + 1);
        assert_reset_path(i + 1, true);
        assert(!s_timer.active);
    }
    if (!initialize_usb) {
        assert(s_restart_timer == NULL);
    }
}

static void cleanup_older(void)
{
    assert(s_cleanup_count == 1);
    s_cleanup_order[s_cleanup_count++] = 1;
    // The shutdown hook must reapply the mode after cleanup, not reset early.
    s_lp_system_reg &= ~LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT;
}

static void cleanup_newer(void)
{
    assert(s_cleanup_count == 0);
    assert((s_lp_system_reg & LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT) != 0);
    s_cleanup_order[s_cleanup_count++] = 2;
}

static void test_shutdown_order(void)
{
    assert(esp_vision_restart_init() == ESP_OK);
    assert(esp_register_shutdown_handler(cleanup_older) == ESP_OK);
    assert(esp_register_shutdown_handler(cleanup_newer) == ESP_OK);
    run_restart(machine_bootloader_rtc);
    assert(s_cleanup_order[0] == 2);
    assert(s_cleanup_order[1] == 1);
#if CONFIG_IDF_TARGET_ESP32S31
    assert(s_cleanup_count == 3);
    assert(s_cleanup_order[2] == 3);
#else
    assert(s_cleanup_count == 2);
#endif
    assert_reset_path(1, true);
    assert((s_lp_system_reg & LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT) != 0);
    assert(s_restart_timer == NULL);
}

static void dummy_0(void) {}
static void dummy_1(void) {}
static void dummy_2(void) {}
static void dummy_3(void) {}
static void dummy_4(void) {}

static void fill_shutdown_handlers(void)
{
    assert(esp_register_shutdown_handler(dummy_0) == ESP_OK);
    assert(esp_register_shutdown_handler(dummy_1) == ESP_OK);
    assert(esp_register_shutdown_handler(dummy_2) == ESP_OK);
    assert(esp_register_shutdown_handler(dummy_3) == ESP_OK);
    assert(esp_register_shutdown_handler(dummy_4) == ESP_OK);
}

static void test_registration(void)
{
    assert(esp_vision_restart_init() == ESP_OK);
    assert(esp_vision_restart_init() == ESP_OK);
    assert(s_register_count == 1);
    assert(s_restart_timer == NULL);
    assert(esp_vision_usb_auto_download_init() == ESP_OK);
    assert(esp_vision_usb_auto_download_init() == ESP_OK);
    assert(s_register_count == 1);
    assert(s_restart_timer != NULL);
}

static void test_registration_full(void)
{
    fill_shutdown_handlers();
    assert(esp_vision_restart_init() == ESP_ERR_NO_MEM);
    assert(!s_restart_initialized);
    assert(s_restart_timer == NULL);
    assert(s_restart_count == 0);
    assert(s_lp_system_reg == 0);
    // A failed registration must not be cached as a successful initialization.
    s_shutdown_handlers[4] = NULL;
    assert(esp_vision_restart_init() == ESP_OK);
    assert(s_restart_initialized);
    assert(s_register_count == 7);
    assert(esp_vision_restart_init() == ESP_OK);
    assert(s_register_count == 7);
}

static void test_auto_init_full(void)
{
    fill_shutdown_handlers();
    assert(esp_vision_usb_auto_download_init() == ESP_ERR_NO_MEM);
    assert(!s_restart_initialized);
    assert(s_restart_timer == NULL);
    assert(s_timer.callback == NULL);
    assert(s_restart_count == 0);
    assert(s_lp_system_reg == 0);
}

static void test_rom_reset_returns(void)
{
    assert(esp_vision_restart_init() == ESP_OK);
    s_rom_reset_returns = true;
    run_restart(machine_bootloader_rtc);
    assert(false);
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
    } else if (strcmp(argv[1], "shutdown-order") == 0) {
        test_shutdown_order();
    } else if (strcmp(argv[1], "registration") == 0) {
        test_registration();
    } else if (strcmp(argv[1], "registration-full") == 0) {
        test_registration_full();
    } else if (strcmp(argv[1], "auto-init-full") == 0) {
        test_auto_init_full();
    } else if (strcmp(argv[1], "rom-return") == 0) {
        test_rom_reset_returns();
    } else if (strcmp(argv[1], "control-full") == 0) {
        test_complete_control_sequence(false);
    } else if (strcmp(argv[1], "control-coalesced") == 0) {
        test_complete_control_sequence(true);
    } else if (strcmp(argv[1], "false-false-open") == 0) {
        test_false_false_open();
    } else {
        assert(false);
    }
    return 0;
}

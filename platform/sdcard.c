/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "sdcard.h"

#include "esp_log.h"
#include "extmod/vfs.h"
#include "modmachine.h"
#include "py/mpconfig.h"
#include "py/nlr.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "boardconfig.h"

#ifndef ESP_VISION_SDCARD_MOUNT_PATH
#define ESP_VISION_SDCARD_MOUNT_PATH "/sdcard"
#endif

#ifndef ESP_VISION_SDCARD_SLOT
#define ESP_VISION_SDCARD_SLOT (0)
#endif

#ifndef ESP_VISION_SDCARD_BUS_WIDTH
#define ESP_VISION_SDCARD_BUS_WIDTH (4)
#endif

static const char *TAG = "esp_vision_sdcard";

MP_WEAK void esp_vision_board_sdcard_init0(void)
{
}

MP_WEAK bool esp_vision_board_sdcard_is_present(void)
{
    return true;
}

MP_WEAK esp_err_t esp_vision_board_sdcard_preinit_host(sdmmc_host_t *host, int slot)
{
    (void)host;
    (void)slot;
    return ESP_OK;
}

MP_WEAK void esp_vision_board_sdcard_deinit_host(sdmmc_host_t *host, int slot)
{
    (void)host;
    (void)slot;
}

void esp_vision_sdcard_init0(void)
{
    esp_vision_board_sdcard_init0();
}

bool esp_vision_sdcard_is_present(void)
{
    return esp_vision_board_sdcard_is_present();
}

esp_err_t esp_vision_sdcard_preinit_host(sdmmc_host_t *host, int slot)
{
    if ((host == NULL) || (slot != ESP_VISION_SDCARD_SLOT)) {
        return ESP_OK;
    }

    if (!esp_vision_sdcard_is_present()) {
        return ESP_ERR_NOT_FOUND;
    }

    return esp_vision_board_sdcard_preinit_host(host, slot);
}

void esp_vision_sdcard_deinit_host(sdmmc_host_t *host, int slot)
{
    if ((host == NULL) || (slot != ESP_VISION_SDCARD_SLOT)) {
        return;
    }

    esp_vision_board_sdcard_deinit_host(host, slot);
}

void esp_vision_sdcard_mount_if_present(void)
{
#if MICROPY_HW_ENABLE_SDCARD
    if (!esp_vision_sdcard_is_present()) {
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        const char *path = ESP_VISION_SDCARD_MOUNT_PATH;
        mp_obj_t sdcard_kwargs[] = {
            MP_OBJ_NEW_QSTR(MP_QSTR_slot),
            MP_OBJ_NEW_SMALL_INT(ESP_VISION_SDCARD_SLOT),
            MP_OBJ_NEW_QSTR(MP_QSTR_width),
            MP_OBJ_NEW_SMALL_INT(ESP_VISION_SDCARD_BUS_WIDTH),
        };
        mp_obj_t sdcard = mp_call_function_n_kw(MP_OBJ_FROM_PTR(&machine_sdcard_type),
                                                0,
                                                2,
                                                sdcard_kwargs);
        mp_obj_t mount_args[] = {
            sdcard,
            mp_obj_new_str(path, strlen(path)),
        };
        mp_vfs_mount(2, mount_args, (mp_map_t *)&mp_const_empty_map);
        nlr_pop();
        ESP_LOGI(TAG, "mounted SD card at %s", path);
    } else {
        mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
        mp_printf(&mp_plat_print, "[esp-vision] SD mount failed: ");
        mp_obj_print_exception(&mp_plat_print, exc);
    }
#endif
}

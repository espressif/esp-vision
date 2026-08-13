/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdcard.h"

#include "py/mpconfig.h"

#include "boardconfig.h"

#ifndef ESP_VISION_SDCARD_SLOT
#define ESP_VISION_SDCARD_SLOT (0)
#endif

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

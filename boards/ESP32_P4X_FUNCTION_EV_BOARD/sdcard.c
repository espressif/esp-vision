/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdcard.h"

#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "boardconfig.h"

static sd_pwr_ctrl_handle_t s_sdcard_ldo_handle = NULL;

void esp_vision_board_sdcard_init0(void)
{
}

bool esp_vision_board_sdcard_is_present(void)
{
    return true;
}

esp_err_t esp_vision_board_sdcard_preinit_host(sdmmc_host_t *host, int slot)
{
    if ((host == NULL) || (slot != ESP_VISION_SDCARD_SLOT)) {
        return ESP_OK;
    }

    host->slot = ESP_VISION_SDCARD_SLOT;
    host->max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    if (s_sdcard_ldo_handle != NULL) {
        host->pwr_ctrl_handle = s_sdcard_ldo_handle;
        return ESP_OK;
    }

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = ESP_VISION_SDCARD_LDO_CHAN_ID,
    };
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_sdcard_ldo_handle);
    if (ret == ESP_OK) {
        host->pwr_ctrl_handle = s_sdcard_ldo_handle;
    }
    return ret;
}

void esp_vision_board_sdcard_deinit_host(sdmmc_host_t *host, int slot)
{
    (void)slot;

    if (host != NULL) {
        host->pwr_ctrl_handle = NULL;
    }
}

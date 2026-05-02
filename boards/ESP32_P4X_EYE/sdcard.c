/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdcard.h"

#include "driver/gpio.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "boardconfig.h"

static sd_pwr_ctrl_handle_t s_sdcard_ldo_handle = NULL;

void esp_vision_board_sdcard_init0(void)
{
    const gpio_config_t sd_en_config = {
        .pin_bit_mask = BIT64(ESP_VISION_SDCARD_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&sd_en_config);
    gpio_set_level(ESP_VISION_SDCARD_EN_PIN, ESP_VISION_SDCARD_EN_ACTIVE_LEVEL);

    const gpio_config_t sd_detect_config = {
        .pin_bit_mask = BIT64(ESP_VISION_SDCARD_DETECT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&sd_detect_config);
}

bool esp_vision_board_sdcard_is_present(void)
{
    return gpio_get_level(ESP_VISION_SDCARD_DETECT_PIN) == ESP_VISION_SDCARD_DETECT_PRESENT_LEVEL;
}

esp_err_t esp_vision_board_sdcard_preinit_host(sdmmc_host_t *host, int slot)
{
    if ((host == NULL) || (slot != ESP_VISION_SDCARD_SLOT)) {
        return ESP_OK;
    }

    if (s_sdcard_ldo_handle != NULL) {
        host->pwr_ctrl_handle = s_sdcard_ldo_handle;
        return ESP_OK;
    }

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = ESP_VISION_SDCARD_LDO_CHAN_ID,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret == ESP_OK) {
        s_sdcard_ldo_handle = pwr_ctrl_handle;
        host->pwr_ctrl_handle = pwr_ctrl_handle;
    }
    return ret;
}

void esp_vision_board_sdcard_deinit_host(sdmmc_host_t *host, int slot)
{
    if ((host == NULL) || (slot != ESP_VISION_SDCARD_SLOT)) {
        return;
    }

    host->pwr_ctrl_handle = NULL;
}

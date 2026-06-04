/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdcard.h"

#include "driver/gpio.h"
#include "sdmmc_cmd.h"

#include "boardconfig.h"

static bool s_sdcard_ctrl_inited;

void esp_vision_board_sdcard_init0(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(ESP_VISION_SDCARD_CTRL_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&config);
    gpio_set_level(ESP_VISION_SDCARD_CTRL_PIN, ESP_VISION_SDCARD_CTRL_ACTIVE_LEVEL);
    s_sdcard_ctrl_inited = true;
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

    if (!s_sdcard_ctrl_inited) {
        esp_vision_board_sdcard_init0();
    }

    host->slot = ESP_VISION_SDCARD_SLOT;
    host->max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host->unaligned_multi_block_rw_max_chunk_size = 8;
    return ESP_OK;
}

void esp_vision_board_sdcard_deinit_host(sdmmc_host_t *host, int slot)
{
    (void)host;
    (void)slot;
}

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_PLATFORM_SDCARD_H
#define ESP_VISION_PLATFORM_SDCARD_H

#include <stdbool.h>

#include "esp_err.h"
#include "driver/sdmmc_host.h"

void esp_vision_sdcard_init0(void);
bool esp_vision_sdcard_is_present(void);
esp_err_t esp_vision_sdcard_preinit_host(sdmmc_host_t *host, int slot);
void esp_vision_sdcard_deinit_host(sdmmc_host_t *host, int slot);
void esp_vision_sdcard_mount_if_present(void);

void esp_vision_board_sdcard_init0(void);
bool esp_vision_board_sdcard_is_present(void);
esp_err_t esp_vision_board_sdcard_preinit_host(sdmmc_host_t *host, int slot);
void esp_vision_board_sdcard_deinit_host(sdmmc_host_t *host, int slot);

#endif /* ESP_VISION_PLATFORM_SDCARD_H */

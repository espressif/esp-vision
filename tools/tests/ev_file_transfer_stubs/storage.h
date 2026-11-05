/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "storage_writer.h"
#define ESP_VISION_STORAGE_SDCARD_PATH "/sdcard"
#define ESP_VISION_STORAGE_FLASH_ALIAS "/flash"
bool esp_vision_storage_flash_is_mounted(void);
bool esp_vision_storage_sdcard_is_mounted(void);

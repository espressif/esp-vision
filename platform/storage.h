/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_PLATFORM_STORAGE_H
#define ESP_VISION_PLATFORM_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "boardconfig.h"

#define ESP_VISION_STORAGE_FLASH_PATH "/flash"

/*
 * The board is the single source of truth for where the SD card is mounted:
 * IDF mounts it at ESP_VISION_SDCARD_MOUNT_PATH (from boardconfig.h), and the
 * MicroPython bridge must publish that exact same path, so both sides use
 * this one macro instead of keeping independent literals in sync by hand.
 */
#ifndef ESP_VISION_SDCARD_MOUNT_PATH
#define ESP_VISION_SDCARD_MOUNT_PATH "/sdcard"
#endif
#define ESP_VISION_STORAGE_SDCARD_PATH ESP_VISION_SDCARD_MOUNT_PATH

#define ESP_VISION_STORAGE_BLOCK_SIZE (512U)

esp_err_t esp_vision_storage_init(void);
void esp_vision_storage_mount_micropython(void);

bool esp_vision_storage_flash_is_mounted(void);
bool esp_vision_storage_sdcard_is_mounted(void);

/*
 * Mount the SD card if it is not mounted yet. Called once from
 * esp_vision_storage_init() and again on every soft reset so that a card
 * inserted after boot is picked up without a hard reset. Returns ESP_OK when
 * the card is mounted, otherwise the board-presence or IDF mount error.
 */
esp_err_t esp_vision_storage_sdcard_try_mount(void);

uint32_t esp_vision_storage_flash_block_count(void);
esp_err_t esp_vision_storage_flash_read(uint32_t address, void *buffer, size_t len);
esp_err_t esp_vision_storage_flash_write(uint32_t address, const void *buffer, size_t len);
esp_err_t esp_vision_storage_flash_sync(void);

bool esp_vision_storage_sdcard_is_ready(void);
uint32_t esp_vision_storage_sdcard_block_count(void);
esp_err_t esp_vision_storage_sdcard_read(uint64_t address, void *buffer, size_t len);
esp_err_t esp_vision_storage_sdcard_write(uint64_t address, const void *buffer, size_t len);
esp_err_t esp_vision_storage_sdcard_sync(void);

#endif /* ESP_VISION_PLATFORM_STORAGE_H */

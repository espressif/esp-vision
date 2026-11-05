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
#include "storage_writer.h"
#include "storage_status.h"

#include "boardconfig.h"

#define ESP_VISION_STORAGE_FLASH_PATH "/flash"
#define ESP_VISION_STORAGE_FLASH_ALIAS "/flash"

/* Each medium is optional. Boards may defer either automatic mount while
 * retaining the non-destructive storage.retry() operation. */
#ifndef ESP_VISION_STORAGE_AUTO_MOUNT_FLASH
#define ESP_VISION_STORAGE_AUTO_MOUNT_FLASH (1)
#endif
#ifndef ESP_VISION_STORAGE_AUTO_MOUNT_SD
#define ESP_VISION_STORAGE_AUTO_MOUNT_SD (1)
#endif

/* Explicit board policy: FR_NO_FILESYSTEM may contain damaged/foreign data.
 * Automatic mounts may erase it. storage.retry() always remains mount-only. */
#ifndef ESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM
#define ESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM (1)
#endif
#ifndef ESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM
#define ESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM (0)
#endif

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
bool esp_vision_storage_flash_is_present(void);
bool esp_vision_storage_sdcard_is_mounted(void);

void esp_vision_storage_get_status(esp_vision_storage_volume_t volume, esp_vision_storage_status_t *out);
/* Neither call resets the VM. Publish a newly mounted Python VFS with an
 * explicit soft reset after inspecting the returned operation result. */
esp_err_t esp_vision_storage_retry(esp_vision_storage_volume_t volume);
/* Recovery is limited to unmounted Flash, confirmed by the user. Legacy vfs,
 * SD, mounted volumes, open handles and quarantined writers are not erased. */
esp_err_t esp_vision_storage_format_flash(bool confirmed);

/*
 * Mount the SD card if it is not mounted yet. Called once from
 * esp_vision_storage_init() and again on every soft reset when the board's
 * ESP_VISION_STORAGE_AUTO_MOUNT_SD is enabled, so that a card
 * inserted after boot is picked up without a hard reset. Returns ESP_OK when
 * the card is mounted, otherwise the board-presence or IDF mount error.
 */
esp_err_t esp_vision_storage_sdcard_try_mount(void);
esp_err_t esp_vision_storage_sdcard_auto_mount(void);

uint32_t esp_vision_storage_flash_block_count(void);
esp_err_t esp_vision_storage_flash_read(uint32_t address, void *buffer, size_t len);
esp_err_t esp_vision_storage_flash_write(uint32_t address, const void *buffer, size_t len);
esp_err_t esp_vision_storage_flash_sync(void);

bool esp_vision_storage_sdcard_is_ready(void);
uint32_t esp_vision_storage_sdcard_block_count(void);
esp_err_t esp_vision_storage_sdcard_read(uint64_t address, void *buffer, size_t len);
esp_err_t esp_vision_storage_sdcard_write(uint64_t address, const void *buffer, size_t len);
esp_err_t esp_vision_storage_sdcard_sync(void);
/* Drop and recreate the MicroPython FAT view after an exclusive raw-block
 * writer handoff. Caller must first prove no VFS handles are open. */
esp_err_t esp_vision_storage_refresh_volume(esp_vision_storage_volume_t volume);
uint32_t esp_vision_storage_vfs_open_handle_count(esp_vision_storage_volume_t volume);

#endif /* ESP_VISION_PLATFORM_STORAGE_H */

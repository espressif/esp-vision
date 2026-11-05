/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_STORAGE_PROVISION_H
#define ESP_VISION_STORAGE_PROVISION_H

#include <stddef.h>

#include "esp_err.h"
#include "esp_partition.h"
#include "ff.h"
#include "storage_status.h"

/* confirmed_format is accepted only for the dedicated data/FAT ffat and an
 * explicit user decision. format_if_no_filesystem is a separate board policy
 * restricted to FR_NO_FILESYSTEM; generic mount/I/O errors never authorize it.
 * An interrupted format stays blocked until a new explicit recovery request. */
esp_err_t esp_vision_storage_mount_flash_fat(const esp_partition_t *partition,
                                             FATFS *filesystem,
                                             const char *drive,
                                             void *work,
                                             size_t work_size,
                                             bool confirmed_format,
                                             bool format_if_no_filesystem,
                                             esp_vision_storage_status_t *status);

/* SD automatic initialization follows the same exact FatFs result gate.
 * Unlike internal Flash, removable SD has no persistent format journal. */
esp_err_t esp_vision_storage_mount_sd_fat(FATFS *filesystem, const char *drive,
                                          void *work, size_t work_size,
                                          bool format_if_no_filesystem,
                                          esp_vision_storage_status_t *status);

#endif /* ESP_VISION_STORAGE_PROVISION_H */

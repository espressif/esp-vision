/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ff.h"

static const char *TAG = "esp_vision_storage";

esp_err_t esp_vision_storage_set_default_label(esp_vision_storage_volume_t volume,
                                               const char *drive, const char *label)
{
    // Numeric FatFs drive IDs are independent of the fixed USB LUN numbers.
    if (drive == NULL || label == NULL || strlen(drive) != 2 ||
            drive[0] < '0' || drive[0] > '9' || drive[1] != ':' ||
            label[0] == '\0' || strlen(label) > 11) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const char *ch = label; *ch != '\0'; ++ch) {
        if (!(*ch >= 'A' && *ch <= 'Z') && !(*ch >= '0' && *ch <= '9') && *ch != '_') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!esp_vision_msc_writer_vfs_begin(volume, true)) {
        ESP_LOGW(TAG, "default label deferred for %s: volume writer unavailable", drive);
        return ESP_ERR_INVALID_STATE;
    }

    // FAT's 11 OEM bytes can expand to UTF-8; exFAT labels can also expand.
    // Reserve four API code units per label character plus the terminator.
    TCHAR current[11 * 4 + 1];
    FRESULT result = f_getlabel(drive, current, NULL);
    if (result == FR_OK && current[0] == '\0') {
        char target[2 + 11 + 1];
        (void)snprintf(target, sizeof(target), "%s%s", drive, label);
        result = f_setlabel(target);
        if (result == FR_OK) {
            ESP_LOGI(TAG, "set default volume label %s on %s", label, drive);
        }
    }
    bool finished = esp_vision_msc_writer_vfs_finish(volume);
    if (result != FR_OK || !finished) {
        ESP_LOGW(TAG, "default volume label failed for %s: FatFs %d, writer finished %d; no format attempted",
                 drive, (int)result, finished);
        return ESP_FAIL;
    }
    return ESP_OK;
}

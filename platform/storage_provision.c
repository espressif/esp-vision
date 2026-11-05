/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage_provision.h"
#include "storage_safety.h"
#include "storage.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define STORAGE_NAMESPACE "ev_storage"
#define STORAGE_RECORD_KEY "flash"
#define STORAGE_RECORD_MAGIC UINT32_C(0x45564653)
#define STORAGE_RECORD_VERSION 1U
#define STORAGE_ERASE_SIZE 4096U

typedef enum {
    STORAGE_PROVISION_PENDING = 1,
    STORAGE_PROVISION_READY = 2,
} storage_provision_state_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t address;
    uint32_t size;
    uint32_t state;
} storage_provision_record_t;

static const char *TAG = "esp_vision_storage";

static esp_err_t storage_load_record(storage_provision_record_t *record)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    size_t length = sizeof(*record);
    ret = nvs_get_blob(handle, STORAGE_RECORD_KEY, record, &length);
    nvs_close(handle);
    if (ret == ESP_OK && (length != sizeof(*record) || record->magic != STORAGE_RECORD_MAGIC ||
                          record->version != STORAGE_RECORD_VERSION ||
                          (record->state != STORAGE_PROVISION_PENDING && record->state != STORAGE_PROVISION_READY))) {
        return ESP_ERR_INVALID_STATE;
    }
    return ret;
}

static esp_err_t storage_save_record(const esp_partition_t *partition, storage_provision_state_t state)
{
    const storage_provision_record_t record = {
        .magic = STORAGE_RECORD_MAGIC,
        .version = STORAGE_RECORD_VERSION,
        .address = partition->address,
        .size = partition->size,
        .state = state,
    };
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_blob(handle, STORAGE_RECORD_KEY, &record, sizeof(record));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

typedef struct {
    const esp_partition_t *partition;
    esp_err_t error;
} storage_read_context_t;

static bool storage_read_partition(void *context, size_t offset, uint8_t *buffer, size_t length)
{
    storage_read_context_t *reader = context;
    reader->error = esp_partition_read(reader->partition, offset, buffer, length);
    return reader->error == ESP_OK;
}

esp_err_t esp_vision_storage_mount_flash_fat(const esp_partition_t *partition,
                                             FATFS *filesystem,
                                             const char *drive,
                                             void *work,
                                             size_t work_size,
                                             bool confirmed_format,
                                             bool format_if_no_filesystem,
                                             esp_vision_storage_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = (esp_vision_storage_status_t) {
        .state = ESP_VISION_STORAGE_UNAVAILABLE, .error = ESP_ERR_INVALID_ARG,
        .fatfs_result = -1, .supported = true,
    };
    if (partition == NULL || filesystem == NULL || drive == NULL || work == NULL ||
            work_size < STORAGE_ERASE_SIZE || partition->size == 0 || partition->size % STORAGE_ERASE_SIZE != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bool factory_ffat = partition->type == ESP_PARTITION_TYPE_DATA &&
                        partition->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT && strcmp(partition->label, "ffat") == 0;
    status->present = true;
    status->address = partition->address;
    status->size = partition->size;
    status->format_supported = factory_ffat;
    if (confirmed_format && !factory_ffat) {
        status->error = ESP_ERR_NOT_SUPPORTED;
        return status->error;
    }
    // The record represents an explicit formatting transaction, not permission
    // to claim unknown media. Absence of a record never authorizes formatting.
    storage_provision_record_t record = {0};
    esp_err_t ret = factory_ffat ? storage_load_record(&record) : ESP_ERR_NVS_NOT_FOUND;
    bool no_record = ret == ESP_ERR_NVS_NOT_FOUND;
    if (ret != ESP_OK && !no_record) {
        status->state = ESP_VISION_STORAGE_STATE_ERROR;
        status->error = ret;
        return ret;
    }
    bool same_partition = !no_record && record.address == partition->address && record.size == partition->size;
    bool pending = same_partition && record.state == STORAGE_PROVISION_PENDING;
    if (!confirmed_format && !no_record && record.state == STORAGE_PROVISION_PENDING) {
        status->state = ESP_VISION_STORAGE_FORMAT_INTERRUPTED;
        status->error = ESP_ERR_INVALID_STATE;
        return status->error;
    }
    FRESULT result = f_mount(filesystem, drive, 1);
    status->fatfs_result = result;
    if (result == FR_OK && !pending) {
        status->state = confirmed_format ? ESP_VISION_STORAGE_UNAVAILABLE : ESP_VISION_STORAGE_READY;
        status->mounted = !confirmed_format;
        status->error = confirmed_format ? ESP_ERR_INVALID_STATE : ESP_OK;
        return status->error; // Do not erase a volume that became readable.
    }
    if (result != FR_OK && result != FR_NO_FILESYSTEM && result != FR_INT_ERR) {
        status->state = ESP_VISION_STORAGE_IO_ERROR;
        status->error = ESP_FAIL;
        return status->error;
    }
    bool automatic_format = !confirmed_format && format_if_no_filesystem && factory_ffat && result == FR_NO_FILESYSTEM;
    if (!confirmed_format && !automatic_format) {
        storage_read_context_t reader = {.partition = partition};
        bool erased = result == FR_NO_FILESYSTEM &&
                      esp_vision_storage_partition_erased(partition->size, work, work_size,
                                                          storage_read_partition, &reader);
        status->state = reader.error != ESP_OK ? ESP_VISION_STORAGE_IO_ERROR :
                        erased ? ESP_VISION_STORAGE_BLANK : ESP_VISION_STORAGE_UNRECOGNIZED;
        status->error = reader.error != ESP_OK ? reader.error : ESP_ERR_INVALID_STATE;
        ESP_LOGW(TAG, "%s", esp_vision_storage_status_message(status->state));
        return status->error;
    }
    if (automatic_format) {
        if (esp_vision_storage_vfs_open_handle_count(ESP_VISION_STORAGE_VOLUME_FLASH) != 0 ||
                !esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, true)) {
            status->state = ESP_VISION_STORAGE_UNAVAILABLE;
            status->error = ESP_ERR_INVALID_STATE;
            return status->error;
        }
        ESP_LOGW(TAG, "FR_NO_FILESYSTEM: board policy formats ffat at 0x%lx, size 0x%lx; existing data is erased",
                 (unsigned long)partition->address, (unsigned long)partition->size);
    }
    // Commit intent before the first destructive write. Reboots do not replay
    // this operation: PENDING requires another explicit user decision.
    ret = storage_save_record(partition, STORAGE_PROVISION_PENDING);
    if (ret != ESP_OK) {
        status->state = ESP_VISION_STORAGE_STATE_ERROR;
        status->error = ret;
        goto format_finished;
    }
    status->state = ESP_VISION_STORAGE_FORMATTING;
    const MKFS_PARM options = {
        .fmt = FM_FAT | FM_SFD,
        .n_fat = 2,
        .align = 0,
        .n_root = 0,
        .au_size = STORAGE_ERASE_SIZE,
    };
    result = f_mkfs(drive, &options, work, work_size);
    if (result == FR_OK) {
        result = f_mount(filesystem, drive, 1);
    }
    if (result != FR_OK) {
        status->state = ESP_VISION_STORAGE_FORMAT_INTERRUPTED;
        status->fatfs_result = result;
        status->error = ESP_FAIL;
        goto format_finished;
    }
    ret = storage_save_record(partition, STORAGE_PROVISION_READY);
    status->state = ret == ESP_OK ? ESP_VISION_STORAGE_READY : ESP_VISION_STORAGE_FORMAT_INTERRUPTED;
    status->mounted = ret == ESP_OK;
    status->error = ret;
    status->fatfs_result = result;
format_finished:
    if (automatic_format && !esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH)) {
        status->state = ESP_VISION_STORAGE_UNAVAILABLE;
        status->mounted = false;
        status->error = ESP_ERR_INVALID_STATE;
    }
    return status->error;
}

esp_err_t esp_vision_storage_mount_sd_fat(FATFS *filesystem, const char *drive,
                                          void *work, size_t work_size,
                                          bool format_if_no_filesystem,
                                          esp_vision_storage_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = (esp_vision_storage_status_t) {
        .state = ESP_VISION_STORAGE_UNAVAILABLE, .error = ESP_ERR_INVALID_ARG,
        .fatfs_result = -1, .supported = true, .present = true,
    };
    if (filesystem == NULL || drive == NULL || work == NULL || work_size < STORAGE_ERASE_SIZE) {
        return status->error;
    }
    FRESULT result = f_mount(filesystem, drive, 1);
    status->fatfs_result = result;
    if (result == FR_OK) {
        status->state = ESP_VISION_STORAGE_READY;
        status->error = ESP_OK;
        status->mounted = true;
        return ESP_OK;
    }
    status->state = result == FR_NO_FILESYSTEM || result == FR_INT_ERR
                    ? ESP_VISION_STORAGE_UNRECOGNIZED : ESP_VISION_STORAGE_IO_ERROR;
    status->error = ESP_FAIL;
    if (!format_if_no_filesystem || result != FR_NO_FILESYSTEM) {
        return status->error;
    }
    if (esp_vision_storage_vfs_open_handle_count(ESP_VISION_STORAGE_VOLUME_SD) != 0 ||
            !esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_SD, true)) {
        status->state = ESP_VISION_STORAGE_UNAVAILABLE;
        status->error = ESP_ERR_INVALID_STATE;
        return status->error;
    }
    ESP_LOGW(TAG, "FR_NO_FILESYSTEM: board policy formats SD; existing data is erased");
    const MKFS_PARM options = {
        .fmt = FM_ANY | FM_SFD, .n_fat = 2, .au_size = STORAGE_ERASE_SIZE,
    };
    result = f_mkfs(drive, &options, work, work_size);
    if (result == FR_OK) {
        result = f_mount(filesystem, drive, 1);
    }
    status->fatfs_result = result;
    status->state = result == FR_OK ? ESP_VISION_STORAGE_READY : ESP_VISION_STORAGE_FORMAT_INTERRUPTED;
    status->mounted = result == FR_OK;
    status->error = result == FR_OK ? ESP_OK : ESP_FAIL;
    if (!esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_SD)) {
        status->state = ESP_VISION_STORAGE_UNAVAILABLE;
        status->mounted = false;
        status->error = ESP_ERR_INVALID_STATE;
    }
    return status->error;
}

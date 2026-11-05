/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <string.h>
#include "storage.h"
#include "esp_partition.h"

#define SOC_SDMMC_HOST_SUPPORTED 1
#define MICROPY_HW_ENABLE_SDCARD 1
#define ESP_PARTITION_SUBTYPE_ANY 0xff
#define ESP_ERR_NOT_FOUND 0x107
#define pdTRUE 1
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define ESP_LOGW(...) ((void)0)

static void *s_flash_mutex, *s_sdcard_mutex, *s_recovery_lock;
static bool s_initialized, s_flash_mounted, s_sdcard_mounted, s_initial_vm_mount_done;
static int s_recovery_lock_buffer, s_status_mux;
static esp_vision_storage_status_t s_flash_status;
static const esp_partition_t *s_flash_partition;
static esp_err_t s_sdcard_error;
static int s_sdcard_fatfs_result = -1;
static esp_vision_storage_state_t s_sdcard_mount_state = ESP_VISION_STORAGE_UNAVAILABLE;
static const esp_partition_t partition = {
    .address = 0x710000, .size = 0x8f0000,
    .type = ESP_PARTITION_TYPE_DATA, .subtype = ESP_PARTITION_SUBTYPE_DATA_FAT, .label = "ffat",
};
static unsigned allocation, fail_allocation, flash_calls, sd_calls, bridges;
static bool flash_present, sd_present;
static esp_err_t flash_result, sd_result;
static bool flash_policy, sd_policy;
static const char *mount_paths[3], *base_paths[3];

static void *xSemaphoreCreateMutexStatic(void *buffer)
{
    return buffer;
}

static void *xSemaphoreCreateMutex(void)
{
    return ++allocation == fail_allocation ? NULL : &allocation;
}

static int xSemaphoreTake(void *lock, unsigned timeout)
{
    assert(lock == s_recovery_lock && timeout == 0);
    return pdTRUE;
}

static void xSemaphoreGive(void *lock)
{
    assert(lock == s_recovery_lock);
}

static const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_ANY);
    return flash_present && strcmp(label, "ffat") == 0 ? &partition : NULL;
}

bool esp_vision_sdcard_is_present(void)
{
    return sd_present;
}

bool esp_vision_storage_flash_is_mounted(void)
{
    return s_flash_mounted;
}

bool esp_vision_storage_sdcard_is_mounted(void)
{
    return s_sdcard_mounted;
}

static esp_err_t esp_vision_storage_register_flash_vfs(bool format, bool automatic)
{
    assert(!format && s_flash_mutex != NULL);
    flash_calls++;
    flash_policy = automatic;
    s_flash_mounted = flash_result == ESP_OK;
    return flash_result;
}

static esp_err_t esp_vision_storage_mount_sdcard(bool automatic)
{
    assert(s_sdcard_mutex != NULL);
    sd_calls++;
    sd_policy = automatic;
    s_sdcard_mounted = sd_result == ESP_OK;
    return sd_result;
}

static void esp_vision_storage_mount_volume(const char *base, const char *mount)
{
    assert(bridges < 3);
    base_paths[bridges] = base;
    mount_paths[bridges++] = mount;
}

/* PRODUCTION_FUNCTIONS */

static void reset(void)
{
    allocation = fail_allocation = flash_calls = sd_calls = bridges = 0;
    s_initialized = s_initial_vm_mount_done = s_flash_mounted = s_sdcard_mounted = false;
    s_flash_mutex = s_sdcard_mutex = NULL;
    flash_present = sd_present = true;
    flash_result = sd_result = ESP_OK;
    flash_policy = sd_policy = false;
    s_sdcard_error = ESP_ERR_NOT_FOUND;
}

int main(void)
{
    assert(ESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM == EV_EXPECT_FLASH_FORMAT);
    assert(ESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM == EV_EXPECT_SD_FORMAT);
    reset();
    assert(esp_vision_storage_init() == ESP_OK);
    assert(flash_calls == ESP_VISION_STORAGE_AUTO_MOUNT_FLASH);
    assert(sd_calls == ESP_VISION_STORAGE_AUTO_MOUNT_SD);
    assert(flash_policy == (bool)(ESP_VISION_STORAGE_AUTO_MOUNT_FLASH && ESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM));
    assert(sd_policy == (bool)(ESP_VISION_STORAGE_AUTO_MOUNT_SD && ESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM));
    esp_vision_storage_mount_micropython();
    assert(bridges == 2 * ESP_VISION_STORAGE_AUTO_MOUNT_FLASH + ESP_VISION_STORAGE_AUTO_MOUNT_SD);
    if (ESP_VISION_STORAGE_AUTO_MOUNT_FLASH) {
        assert(strcmp(mount_paths[0], "/") == 0 && strcmp(mount_paths[1], "/flash") == 0);
        assert(strcmp(base_paths[0], "/flash") == 0 && strcmp(base_paths[1], "/flash") == 0);
    } else {
        esp_vision_storage_status_t status;
        esp_vision_storage_get_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status);
        assert(status.state == ESP_VISION_STORAGE_UNMOUNTED && status.present && !status.mounted);
        assert(status.address == partition.address && status.size == partition.size);
    }
    if (ESP_VISION_STORAGE_AUTO_MOUNT_SD) {
        assert(strcmp(mount_paths[bridges - 1], "/sdcard") == 0);
    }
    assert(esp_vision_storage_init() == ESP_OK);
    assert(flash_calls == ESP_VISION_STORAGE_AUTO_MOUNT_FLASH && sd_calls == ESP_VISION_STORAGE_AUTO_MOUNT_SD);
    if (!ESP_VISION_STORAGE_AUTO_MOUNT_SD) {
        esp_vision_storage_status_t status;
        esp_vision_storage_get_status(ESP_VISION_STORAGE_VOLUME_SD, &status);
        assert(status.state == ESP_VISION_STORAGE_UNMOUNTED && status.present && !status.mounted);
    }
    assert(esp_vision_storage_retry(ESP_VISION_STORAGE_VOLUME_FLASH) == ESP_OK);
    assert(esp_vision_storage_retry(ESP_VISION_STORAGE_VOLUME_SD) == ESP_OK);
    assert(flash_calls == 1 && sd_calls == 1);
    if (!ESP_VISION_STORAGE_AUTO_MOUNT_FLASH) {
        assert(!flash_policy);
    }
    if (!ESP_VISION_STORAGE_AUTO_MOUNT_SD) {
        assert(!sd_policy);
    }
    bridges = 0;
    esp_vision_storage_mount_micropython();
    assert(bridges == 3 && flash_calls == 1 && sd_calls == 1);

    reset();
    flash_result = ESP_FAIL;
    esp_vision_storage_init();
    assert(!s_flash_mounted && sd_calls == ESP_VISION_STORAGE_AUTO_MOUNT_SD);
    bridges = 0;
    esp_vision_storage_mount_micropython();
    assert(bridges == ESP_VISION_STORAGE_AUTO_MOUNT_SD);
    if (bridges) {
        assert(strcmp(mount_paths[0], "/sdcard") == 0);
    }

    reset();
    sd_result = ESP_FAIL;
    assert(esp_vision_storage_init() == ESP_OK);
    assert(s_flash_mounted == (bool)ESP_VISION_STORAGE_AUTO_MOUNT_FLASH && !s_sdcard_mounted);
    bridges = 0;
    esp_vision_storage_mount_micropython();
    assert(sd_calls == ESP_VISION_STORAGE_AUTO_MOUNT_SD); // No second cold-boot probe.
    bridges = 0;
    esp_vision_storage_mount_micropython();
    assert(sd_calls == 2 * ESP_VISION_STORAGE_AUTO_MOUNT_SD); // Soft-reset retry honors the board flag.

    reset();
    fail_allocation = 1;
    assert(esp_vision_storage_init() == ESP_ERR_NO_MEM);
    assert(flash_calls == 0 && sd_calls == ESP_VISION_STORAGE_AUTO_MOUNT_SD);
    reset();
    fail_allocation = 2;
    assert(esp_vision_storage_init() == ESP_OK);
    assert(flash_calls == ESP_VISION_STORAGE_AUTO_MOUNT_FLASH && sd_calls == 0);
    assert(esp_vision_storage_sdcard_try_mount() == ESP_ERR_NO_MEM);
    return 0;
}

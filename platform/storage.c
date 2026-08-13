/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage.h"

#include <stdint.h>
#include <string.h>

#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "py/mpconfig.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"

#include "boardconfig.h"
#include "sdcard.h"

#define ESP_VISION_STORAGE_ERASE_BLOCK_SIZE (4096U)
#define ESP_VISION_STORAGE_MAX_FILES        (16)

#ifndef ESP_VISION_SDCARD_SLOT
#define ESP_VISION_SDCARD_SLOT (0)
#endif

#ifndef ESP_VISION_SDCARD_BUS_WIDTH
#define ESP_VISION_SDCARD_BUS_WIDTH (4)
#endif

#if SOC_SDMMC_USE_GPIO_MATRIX && \
    defined(ESP_VISION_SDCARD_CLK_PIN) && \
    defined(ESP_VISION_SDCARD_CMD_PIN) && \
    defined(ESP_VISION_SDCARD_D0_PIN)
#define ESP_VISION_SDCARD_HAS_PIN_CONFIG (1)
#else
#define ESP_VISION_SDCARD_HAS_PIN_CONFIG (0)
#endif

static const char *TAG = "esp_vision_storage";

static const esp_partition_t *s_flash_partition;
static SemaphoreHandle_t s_flash_mutex;
static FATFS *s_flash_fs;
static BYTE s_flash_pdrv = FF_DRV_NOT_USED;
static char s_flash_drive[3];
static bool s_flash_mounted;
static bool s_sdcard_mounted;
static bool s_initialized;
static uint8_t s_flash_erase_cache[ESP_VISION_STORAGE_ERASE_BLOCK_SIZE];
static uint8_t s_flash_mkfs_work[ESP_VISION_STORAGE_ERASE_BLOCK_SIZE];

#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
static sdmmc_card_t *s_sdcard;
static SemaphoreHandle_t s_sdcard_mutex;
static BYTE s_sdcard_pdrv = FF_DRV_NOT_USED;
static uint8_t s_sdcard_sector_cache[ESP_VISION_STORAGE_BLOCK_SIZE] __attribute__((aligned(4)));
#endif

static bool esp_vision_storage_flash_range_valid(uint32_t address, size_t len)
{
    return (s_flash_partition != NULL) &&
           (address <= s_flash_partition->size) &&
           (len <= (s_flash_partition->size - address));
}

esp_err_t esp_vision_storage_flash_read(uint32_t address, void *buffer, size_t len)
{
    if ((buffer == NULL && len != 0) || !esp_vision_storage_flash_range_valid(address, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_flash_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flash_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = esp_partition_read(s_flash_partition, address, buffer, len);
    xSemaphoreGive(s_flash_mutex);
    return ret;
}

esp_err_t esp_vision_storage_flash_write(uint32_t address, const void *buffer, size_t len)
{
    if ((buffer == NULL && len != 0) || !esp_vision_storage_flash_range_valid(address, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_flash_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flash_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint8_t *src = buffer;
    esp_err_t ret = ESP_OK;
    while (len > 0) {
        uint32_t erase_base = (address / ESP_VISION_STORAGE_ERASE_BLOCK_SIZE) * ESP_VISION_STORAGE_ERASE_BLOCK_SIZE;
        uint32_t erase_offset = address - erase_base;
        size_t write_len = ESP_VISION_STORAGE_ERASE_BLOCK_SIZE - erase_offset;
        if (write_len > len) {
            write_len = len;
        }

        if ((erase_offset == 0) && (write_len == ESP_VISION_STORAGE_ERASE_BLOCK_SIZE)) {
            ret = esp_partition_erase_range(s_flash_partition, erase_base, ESP_VISION_STORAGE_ERASE_BLOCK_SIZE);
            if (ret == ESP_OK) {
                ret = esp_partition_write(s_flash_partition, erase_base, src, ESP_VISION_STORAGE_ERASE_BLOCK_SIZE);
            }
        } else {
            ret = esp_partition_read(s_flash_partition,
                                     erase_base,
                                     s_flash_erase_cache,
                                     ESP_VISION_STORAGE_ERASE_BLOCK_SIZE);
            if (ret == ESP_OK) {
                memcpy(s_flash_erase_cache + erase_offset, src, write_len);
                ret = esp_partition_erase_range(s_flash_partition, erase_base, ESP_VISION_STORAGE_ERASE_BLOCK_SIZE);
            }
            if (ret == ESP_OK) {
                ret = esp_partition_write(s_flash_partition,
                                          erase_base,
                                          s_flash_erase_cache,
                                          ESP_VISION_STORAGE_ERASE_BLOCK_SIZE);
            }
        }

        if (ret != ESP_OK) {
            break;
        }
        address += write_len;
        src += write_len;
        len -= write_len;
    }

    xSemaphoreGive(s_flash_mutex);
    return ret;
}

esp_err_t esp_vision_storage_flash_sync(void)
{
    return s_flash_partition == NULL ? ESP_ERR_INVALID_STATE : ESP_OK;
}

uint32_t esp_vision_storage_flash_block_count(void)
{
    return s_flash_partition == NULL ? 0 : s_flash_partition->size / ESP_VISION_STORAGE_BLOCK_SIZE;
}

bool esp_vision_storage_flash_is_mounted(void)
{
    return s_flash_mounted;
}

bool esp_vision_storage_sdcard_is_mounted(void)
{
    return s_sdcard_mounted;
}

#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
static esp_err_t esp_vision_storage_sdcard_lock(void)
{
    if (s_sdcard_mutex == NULL || s_sdcard == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_sdcard_mutex, portMAX_DELAY) == pdTRUE
           ? ESP_OK
           : ESP_ERR_TIMEOUT;
}

static void esp_vision_storage_sdcard_unlock(void)
{
    xSemaphoreGive(s_sdcard_mutex);
}

static bool esp_vision_storage_sdcard_range_valid(uint64_t address, size_t len)
{
    if (!s_sdcard_mounted || s_sdcard == NULL ||
            s_sdcard->csd.sector_size != ESP_VISION_STORAGE_BLOCK_SIZE) {
        return false;
    }
    uint64_t size = (uint64_t)s_sdcard->csd.capacity * s_sdcard->csd.sector_size;
    return address <= size && len <= size - address;
}

static esp_err_t esp_vision_storage_sdcard_read_locked(uint64_t address,
                                                       void *buffer,
                                                       size_t len)
{
    uint8_t *dst = buffer;
    const size_t block_size = s_sdcard->csd.sector_size;

    while (len > 0) {
        size_t block = address / block_size;
        size_t offset = address % block_size;
        size_t chunk = block_size - offset;
        if (chunk > len) {
            chunk = len;
        }

        esp_err_t ret;
        if (offset == 0 && chunk == block_size) {
            size_t block_count = len / block_size;
            ret = sdmmc_read_sectors(s_sdcard, dst, block, block_count);
            chunk = block_count * block_size;
        } else {
            ret = sdmmc_read_sectors(s_sdcard, s_sdcard_sector_cache, block, 1);
            if (ret == ESP_OK) {
                memcpy(dst, s_sdcard_sector_cache + offset, chunk);
            }
        }
        if (ret != ESP_OK) {
            return ret;
        }
        address += chunk;
        dst += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static esp_err_t esp_vision_storage_sdcard_write_locked(uint64_t address,
                                                        const void *buffer,
                                                        size_t len)
{
    const uint8_t *src = buffer;
    const size_t block_size = s_sdcard->csd.sector_size;

    while (len > 0) {
        size_t block = address / block_size;
        size_t offset = address % block_size;
        size_t chunk = block_size - offset;
        if (chunk > len) {
            chunk = len;
        }

        esp_err_t ret;
        if (offset == 0 && chunk == block_size) {
            size_t block_count = len / block_size;
            ret = sdmmc_write_sectors(s_sdcard, src, block, block_count);
            chunk = block_count * block_size;
        } else {
            ret = sdmmc_read_sectors(s_sdcard, s_sdcard_sector_cache, block, 1);
            if (ret == ESP_OK) {
                memcpy(s_sdcard_sector_cache + offset, src, chunk);
                ret = sdmmc_write_sectors(s_sdcard, s_sdcard_sector_cache, block, 1);
            }
        }
        if (ret != ESP_OK) {
            return ret;
        }
        address += chunk;
        src += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static DSTATUS esp_vision_storage_sdcard_disk_status(BYTE pdrv)
{
    if (pdrv != s_sdcard_pdrv || esp_vision_storage_sdcard_lock() != ESP_OK) {
        return STA_NOINIT;
    }
    esp_err_t ret = sdmmc_get_status(s_sdcard);
    esp_vision_storage_sdcard_unlock();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card status failed: %s", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : STA_NOINIT;
}

static DRESULT esp_vision_storage_sdcard_disk_read(BYTE pdrv,
                                                   BYTE *buffer,
                                                   uint32_t sector,
                                                   UINT count)
{
    if (pdrv != s_sdcard_pdrv || buffer == NULL || count == 0 ||
            esp_vision_storage_sdcard_lock() != ESP_OK) {
        return RES_PARERR;
    }
    esp_err_t ret = sdmmc_read_sectors(s_sdcard, buffer, sector, count);
    esp_vision_storage_sdcard_unlock();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card read failed: %s", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT esp_vision_storage_sdcard_disk_write(BYTE pdrv,
                                                    const BYTE *buffer,
                                                    uint32_t sector,
                                                    UINT count)
{
    if (pdrv != s_sdcard_pdrv || buffer == NULL || count == 0 ||
            esp_vision_storage_sdcard_lock() != ESP_OK) {
        return RES_PARERR;
    }
    esp_err_t ret = sdmmc_write_sectors(s_sdcard, buffer, sector, count);
    esp_vision_storage_sdcard_unlock();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card write failed: %s", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT esp_vision_storage_sdcard_disk_ioctl(BYTE pdrv, BYTE command, void *buffer)
{
    if (pdrv != s_sdcard_pdrv || s_sdcard == NULL) {
        return RES_PARERR;
    }

    switch (command) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(DWORD *)buffer = s_sdcard->csd.capacity;
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(WORD *)buffer = s_sdcard->csd.sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        return RES_ERROR;
#if FF_USE_TRIM
    case CTRL_TRIM: {
        if (buffer == NULL || sdmmc_can_trim(s_sdcard) != ESP_OK) {
            return RES_PARERR;
        }
        const DWORD *range = buffer;
        if (range[1] < range[0] || esp_vision_storage_sdcard_lock() != ESP_OK) {
            return RES_PARERR;
        }
        sdmmc_erase_arg_t arg = sdmmc_can_discard(s_sdcard) == ESP_OK
                                ? SDMMC_DISCARD_ARG
                                : SDMMC_ERASE_ARG;
        esp_err_t ret = sdmmc_erase_sectors(s_sdcard,
                                            range[0],
                                            range[1] - range[0] + 1,
                                            arg);
        esp_vision_storage_sdcard_unlock();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SD card trim failed: %s", esp_err_to_name(ret));
        }
        return ret == ESP_OK ? RES_OK : RES_ERROR;
    }
#endif
    default:
        return RES_PARERR;
    }
}

static void esp_vision_storage_register_sdcard_diskio(void)
{
    s_sdcard_pdrv = ff_diskio_get_pdrv_card(s_sdcard);
    if (s_sdcard_pdrv == FF_DRV_NOT_USED) {
        ESP_LOGW(TAG, "failed to locate SD card FatFs drive");
        return;
    }

    const ff_diskio_impl_t diskio = {
        .init = esp_vision_storage_sdcard_disk_status,
        .status = esp_vision_storage_sdcard_disk_status,
        .read = esp_vision_storage_sdcard_disk_read,
        .write = esp_vision_storage_sdcard_disk_write,
        .ioctl = esp_vision_storage_sdcard_disk_ioctl,
    };
    ff_diskio_register(s_sdcard_pdrv, &diskio);
}
#endif

bool esp_vision_storage_sdcard_is_ready(void)
{
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if (!s_sdcard_mounted || esp_vision_storage_sdcard_lock() != ESP_OK) {
        return false;
    }
    esp_err_t ret = sdmmc_get_status(s_sdcard);
    esp_vision_storage_sdcard_unlock();
    return ret == ESP_OK;
#else
    return false;
#endif
}

uint32_t esp_vision_storage_sdcard_block_count(void)
{
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    return s_sdcard_mounted && s_sdcard != NULL &&
           s_sdcard->csd.sector_size == ESP_VISION_STORAGE_BLOCK_SIZE
           ? s_sdcard->csd.capacity
           : 0;
#else
    return 0;
#endif
}

esp_err_t esp_vision_storage_sdcard_read(uint64_t address, void *buffer, size_t len)
{
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if ((buffer == NULL && len != 0) || !esp_vision_storage_sdcard_range_valid(address, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = esp_vision_storage_sdcard_lock();
    if (ret == ESP_OK) {
        ret = esp_vision_storage_sdcard_read_locked(address, buffer, len);
        esp_vision_storage_sdcard_unlock();
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_vision_storage_sdcard_write(uint64_t address, const void *buffer, size_t len)
{
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if ((buffer == NULL && len != 0) || !esp_vision_storage_sdcard_range_valid(address, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = esp_vision_storage_sdcard_lock();
    if (ret == ESP_OK) {
        ret = esp_vision_storage_sdcard_write_locked(address, buffer, len);
        esp_vision_storage_sdcard_unlock();
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_vision_storage_sdcard_sync(void)
{
    return esp_vision_storage_sdcard_block_count() == 0
           ? ESP_ERR_INVALID_STATE
           : ESP_OK;
}

static DSTATUS esp_vision_storage_disk_initialize(BYTE pdrv)
{
    return (pdrv == s_flash_pdrv && s_flash_partition != NULL) ? 0 : STA_NOINIT;
}

static DSTATUS esp_vision_storage_disk_status(BYTE pdrv)
{
    return esp_vision_storage_disk_initialize(pdrv);
}

static DRESULT esp_vision_storage_disk_read(BYTE pdrv, BYTE *buffer, uint32_t sector, UINT count)
{
    uint64_t address = (uint64_t)sector * ESP_VISION_STORAGE_BLOCK_SIZE;
    uint64_t len = (uint64_t)count * ESP_VISION_STORAGE_BLOCK_SIZE;
    if ((pdrv != s_flash_pdrv) || (count == 0) || (address > UINT32_MAX) || (len > SIZE_MAX)) {
        return RES_PARERR;
    }
    return esp_vision_storage_flash_read((uint32_t)address, buffer, (size_t)len) == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT esp_vision_storage_disk_write(BYTE pdrv, const BYTE *buffer, uint32_t sector, UINT count)
{
    uint64_t address = (uint64_t)sector * ESP_VISION_STORAGE_BLOCK_SIZE;
    uint64_t len = (uint64_t)count * ESP_VISION_STORAGE_BLOCK_SIZE;
    if ((pdrv != s_flash_pdrv) || (count == 0) || (address > UINT32_MAX) || (len > SIZE_MAX)) {
        return RES_PARERR;
    }
    return esp_vision_storage_flash_write((uint32_t)address, buffer, (size_t)len) == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT esp_vision_storage_disk_ioctl(BYTE pdrv, BYTE command, void *buffer)
{
    if (pdrv != s_flash_pdrv) {
        return RES_PARERR;
    }

    switch (command) {
    case CTRL_SYNC:
        return esp_vision_storage_flash_sync() == ESP_OK ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(DWORD *)buffer = esp_vision_storage_flash_block_count();
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(WORD *)buffer = ESP_VISION_STORAGE_BLOCK_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(DWORD *)buffer = ESP_VISION_STORAGE_ERASE_BLOCK_SIZE / ESP_VISION_STORAGE_BLOCK_SIZE;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static void esp_vision_storage_unregister_flash(void)
{
    if (s_flash_fs != NULL) {
        f_mount(NULL, s_flash_drive, 0);
        esp_vfs_fat_unregister_path(ESP_VISION_STORAGE_FLASH_PATH);
        s_flash_fs = NULL;
    }
    if (s_flash_pdrv != FF_DRV_NOT_USED) {
        ff_diskio_unregister(s_flash_pdrv);
        s_flash_pdrv = FF_DRV_NOT_USED;
    }
}

static esp_err_t esp_vision_storage_register_flash_vfs(void)
{
    s_flash_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                 ESP_PARTITION_SUBTYPE_ANY,
                                                 "ffat");
    bool allow_format = s_flash_partition != NULL;
    if (s_flash_partition == NULL) {
        s_flash_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                     ESP_PARTITION_SUBTYPE_ANY,
                                                     "vfs");
    }
    if (s_flash_partition == NULL ||
            (s_flash_partition->size % ESP_VISION_STORAGE_ERASE_BLOCK_SIZE) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = ff_diskio_get_drive(&s_flash_pdrv);
    if (ret != ESP_OK) {
        return ret;
    }
    s_flash_drive[0] = '0' + s_flash_pdrv;
    s_flash_drive[1] = ':';
    s_flash_drive[2] = '\0';

    const ff_diskio_impl_t diskio = {
        .init = esp_vision_storage_disk_initialize,
        .status = esp_vision_storage_disk_status,
        .read = esp_vision_storage_disk_read,
        .write = esp_vision_storage_disk_write,
        .ioctl = esp_vision_storage_disk_ioctl,
    };
    ff_diskio_register(s_flash_pdrv, &diskio);

    const esp_vfs_fat_conf_t conf = {
        .base_path = ESP_VISION_STORAGE_FLASH_PATH,
        .fat_drive = s_flash_drive,
        .max_files = ESP_VISION_STORAGE_MAX_FILES,
    };
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    ret = esp_vfs_fat_register(&conf, &s_flash_fs);
#else
    ret = esp_vfs_fat_register_cfg(&conf, &s_flash_fs);
#endif
    if (ret != ESP_OK) {
        esp_vision_storage_unregister_flash();
        return ret;
    }

    FRESULT result = f_mount(s_flash_fs, s_flash_drive, 1);
    if ((result == FR_NO_FILESYSTEM || result == FR_INT_ERR) && allow_format) {
        ESP_LOGW(TAG, "formatting unmountable ffat partition");
        const MKFS_PARM options = {
            .fmt = FM_FAT | FM_SFD,
            .n_fat = 2,
            .align = 0,
            .n_root = 0,
            .au_size = ESP_VISION_STORAGE_ERASE_BLOCK_SIZE,
        };
        result = f_mkfs(s_flash_drive, &options, s_flash_mkfs_work, sizeof(s_flash_mkfs_work));
        if (result == FR_OK) {
            result = f_mount(s_flash_fs, s_flash_drive, 1);
        }
    }

    if (result != FR_OK) {
        ESP_LOGE(TAG, "failed to mount Flash FAT filesystem: %d", result);
        esp_vision_storage_unregister_flash();
        return ESP_FAIL;
    }

    s_flash_mounted = true;
    ESP_LOGI(TAG, "mounted Flash at %s", ESP_VISION_STORAGE_FLASH_PATH);
    return ESP_OK;
}

#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) && \
    defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) && defined(CONFIG_ESP_HOSTED_SDIO_SLOT)
static esp_err_t esp_vision_storage_sdmmc_host_init_dummy(void)
{
    return ESP_OK;
}

static esp_err_t esp_vision_storage_sdmmc_host_deinit_dummy(void)
{
    return ESP_OK;
}
#endif

static esp_err_t esp_vision_storage_mount_sdcard(void)
{
    esp_vision_sdcard_init0();
    if (!esp_vision_sdcard_is_present()) {
        return ESP_ERR_NOT_FOUND;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = ESP_VISION_SDCARD_SLOT;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) && \
    defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) && defined(CONFIG_ESP_HOSTED_SDIO_SLOT)
    if (ESP_VISION_SDCARD_SLOT != CONFIG_ESP_HOSTED_SDIO_SLOT) {
        host.init = esp_vision_storage_sdmmc_host_init_dummy;
        host.deinit = esp_vision_storage_sdmmc_host_deinit_dummy;
    }
#endif

    esp_err_t ret = esp_vision_sdcard_preinit_host(&host, ESP_VISION_SDCARD_SLOT);
    if (ret != ESP_OK) {
        return ret;
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = ESP_VISION_SDCARD_BUS_WIDTH;
#if ESP_VISION_SDCARD_HAS_PIN_CONFIG
    slot.clk = ESP_VISION_SDCARD_CLK_PIN;
    slot.cmd = ESP_VISION_SDCARD_CMD_PIN;
    slot.d0 = ESP_VISION_SDCARD_D0_PIN;
#if ESP_VISION_SDCARD_BUS_WIDTH == 4 && \
    defined(ESP_VISION_SDCARD_D1_PIN) && \
    defined(ESP_VISION_SDCARD_D2_PIN) && \
    defined(ESP_VISION_SDCARD_D3_PIN)
    slot.d1 = ESP_VISION_SDCARD_D1_PIN;
    slot.d2 = ESP_VISION_SDCARD_D2_PIN;
    slot.d3 = ESP_VISION_SDCARD_D3_PIN;
#elif ESP_VISION_SDCARD_BUS_WIDTH != 1
#error "ESP_VISION_SDCARD_BUS_WIDTH requires matching ESP_VISION_SDCARD_Dx_PIN definitions"
#endif
#endif

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = ESP_VISION_STORAGE_MAX_FILES,
        .allocation_unit_size = 0,
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };
    ret = esp_vfs_fat_sdmmc_mount(ESP_VISION_SDCARD_MOUNT_PATH,
                                  &host,
                                  &slot,
                                  &mount_config,
                                  &s_sdcard);
    if (ret != ESP_OK) {
        esp_vision_sdcard_deinit_host(&host, ESP_VISION_SDCARD_SLOT);
        return ret;
    }

    esp_vision_storage_register_sdcard_diskio();
    s_sdcard_mounted = true;
    ESP_LOGI(TAG, "mounted SD card at %s", ESP_VISION_SDCARD_MOUNT_PATH);
    return ESP_OK;
}
#endif

esp_err_t esp_vision_storage_sdcard_try_mount(void)
{
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if (s_sdcard_mounted) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vision_storage_mount_sdcard();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_vision_storage_init(void)
{
    if (s_initialized) {
        return s_flash_mounted ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    s_initialized = true;

    s_flash_mutex = xSemaphoreCreateMutex();
    if (s_flash_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    s_sdcard_mutex = xSemaphoreCreateMutex();
    if (s_sdcard_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
#endif

    esp_err_t flash_ret = esp_vision_storage_register_flash_vfs();

    esp_vision_storage_sdcard_try_mount();

    return flash_ret;
}

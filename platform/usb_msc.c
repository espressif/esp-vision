/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "py/mpconfig.h"

#if MICROPY_HW_ENABLE_USBDEV && MICROPY_HW_USB_MSC

#include <stdint.h>
#include <string.h>

#include "soc/soc_caps.h"
#include "tusb.h"

#include "storage.h"

#define ESP_VISION_MSC_FLASH_LUN (0)
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
#define ESP_VISION_MSC_SDCARD_LUN (1)
#define ESP_VISION_MSC_LUN_COUNT  (2)
#define ESP_VISION_MSC_SDCARD_PRODUCT_STRING "ESP-VISION SD"
#else
#define ESP_VISION_MSC_LUN_COUNT (1)
#endif

static bool s_msc_ejected[ESP_VISION_MSC_LUN_COUNT];

static bool esp_vision_msc_lun_valid(uint8_t lun)
{
    return lun < ESP_VISION_MSC_LUN_COUNT;
}

static uint32_t esp_vision_msc_block_count(uint8_t lun)
{
    if (lun == ESP_VISION_MSC_FLASH_LUN) {
        return esp_vision_storage_flash_block_count();
    }
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if (lun == ESP_VISION_MSC_SDCARD_LUN) {
        return esp_vision_storage_sdcard_block_count();
    }
#endif
    return 0;
}

static bool esp_vision_msc_lun_ready(uint8_t lun)
{
    if (!esp_vision_msc_lun_valid(lun) || s_msc_ejected[lun]) {
        return false;
    }
    if (lun == ESP_VISION_MSC_FLASH_LUN) {
        return esp_vision_storage_flash_block_count() != 0;
    }
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if (lun == ESP_VISION_MSC_SDCARD_LUN) {
        return esp_vision_storage_sdcard_is_ready();
    }
#endif
    return false;
}

static esp_err_t esp_vision_msc_sync(uint8_t lun)
{
    if (lun == ESP_VISION_MSC_FLASH_LUN) {
        return esp_vision_storage_flash_sync();
    }
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if (lun == ESP_VISION_MSC_SDCARD_LUN) {
        return esp_vision_storage_sdcard_sync();
    }
#endif
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t esp_vision_msc_read(uint8_t lun,
                                     uint64_t address,
                                     void *buffer,
                                     size_t len)
{
    if (lun == ESP_VISION_MSC_FLASH_LUN) {
        return address <= UINT32_MAX
               ? esp_vision_storage_flash_read((uint32_t)address, buffer, len)
               : ESP_ERR_INVALID_SIZE;
    }
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if (lun == ESP_VISION_MSC_SDCARD_LUN) {
        return esp_vision_storage_sdcard_read(address, buffer, len);
    }
#endif
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t esp_vision_msc_write(uint8_t lun,
                                      uint64_t address,
                                      const void *buffer,
                                      size_t len)
{
    if (lun == ESP_VISION_MSC_FLASH_LUN) {
        return address <= UINT32_MAX
               ? esp_vision_storage_flash_write((uint32_t)address, buffer, len)
               : ESP_ERR_INVALID_SIZE;
    }
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if (lun == ESP_VISION_MSC_SDCARD_LUN) {
        return esp_vision_storage_sdcard_write(address, buffer, len);
    }
#endif
    return ESP_ERR_INVALID_ARG;
}

static void esp_vision_msc_copy_inquiry_string(uint8_t *dst, size_t dst_len, const char *src)
{
    memset(dst, ' ', dst_len);
    if (src == NULL) {
        return;
    }

    size_t src_len = strlen(src);
    if (src_len > dst_len) {
        src_len = dst_len;
    }
    memcpy(dst, src, src_len);
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    const char *product = MICROPY_HW_USB_MSC_INQUIRY_PRODUCT_STRING;
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if (lun == ESP_VISION_MSC_SDCARD_LUN) {
        product = ESP_VISION_MSC_SDCARD_PRODUCT_STRING;
    }
#endif
    esp_vision_msc_copy_inquiry_string(vendor_id, 8, MICROPY_HW_USB_MSC_INQUIRY_VENDOR_STRING);
    esp_vision_msc_copy_inquiry_string(product_id, 16, product);
    esp_vision_msc_copy_inquiry_string(product_rev, 4, MICROPY_HW_USB_MSC_INQUIRY_REVISION_STRING);
}

uint8_t tud_msc_get_maxlun_cb(void)
{
    return ESP_VISION_MSC_LUN_COUNT;
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!esp_vision_msc_lun_ready(lun)) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    *block_size = ESP_VISION_STORAGE_BLOCK_SIZE;
    *block_count = esp_vision_msc_block_count(lun);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)power_condition;

    if (!esp_vision_msc_lun_valid(lun)) {
        return false;
    }
    if (load_eject) {
        if (!start) {
            if (esp_vision_msc_sync(lun) != ESP_OK) {
                return false;
            }
        }
        s_msc_ejected[lun] = !start;
    } else if (!start && esp_vision_msc_sync(lun) != ESP_OK) {
        return false;
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    return esp_vision_msc_lun_ready(lun);
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    uint64_t address = ((uint64_t)lba * ESP_VISION_STORAGE_BLOCK_SIZE) + offset;
    if (!esp_vision_msc_lun_ready(lun) ||
            esp_vision_msc_read(lun, address, buffer, bufsize) != ESP_OK) {
        return -1;
    }

    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    uint64_t address = ((uint64_t)lba * ESP_VISION_STORAGE_BLOCK_SIZE) + offset;
    return esp_vision_msc_lun_ready(lun) &&
           esp_vision_msc_write(lun, address, buffer, bufsize) == ESP_OK
           ? (int32_t)bufsize
           : -1;
}

void tud_msc_write10_complete_cb(uint8_t lun)
{
    if (esp_vision_msc_lun_valid(lun)) {
        (void)esp_vision_msc_sync(lun);
    }
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

#endif /* MICROPY_HW_ENABLE_USBDEV && MICROPY_HW_USB_MSC */

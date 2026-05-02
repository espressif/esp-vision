/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "py/mpconfig.h"

#if MICROPY_HW_ENABLE_USBDEV && MICROPY_HW_USB_MSC

#include <stdint.h>
#include <string.h>

#include "esp_partition.h"
#include "tusb.h"

#define ESP_VISION_MSC_BLOCK_SIZE       (512)
#define ESP_VISION_MSC_ERASE_BLOCK_SIZE (4096)

static const esp_partition_t *s_msc_partition = NULL;
static bool s_msc_ejected = false;
static uint8_t s_msc_erase_cache[ESP_VISION_MSC_ERASE_BLOCK_SIZE];

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

static const esp_partition_t *esp_vision_msc_get_partition(void)
{
    if (s_msc_partition == NULL) {
        s_msc_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "ffat");
        if (s_msc_partition == NULL) {
            s_msc_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "vfs");
        }
    }
    return s_msc_partition;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    esp_vision_msc_copy_inquiry_string(vendor_id, 8, MICROPY_HW_USB_MSC_INQUIRY_VENDOR_STRING);
    esp_vision_msc_copy_inquiry_string(product_id, 16, MICROPY_HW_USB_MSC_INQUIRY_PRODUCT_STRING);
    esp_vision_msc_copy_inquiry_string(product_rev, 4, MICROPY_HW_USB_MSC_INQUIRY_REVISION_STRING);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (s_msc_ejected || (esp_vision_msc_get_partition() == NULL)) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;

    const esp_partition_t *partition = esp_vision_msc_get_partition();
    *block_size = ESP_VISION_MSC_BLOCK_SIZE;
    *block_count = (partition == NULL) ? 0 : (partition->size / ESP_VISION_MSC_BLOCK_SIZE);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;

    if (load_eject) {
        s_msc_ejected = !start;
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;

    const esp_partition_t *partition = esp_vision_msc_get_partition();
    if (partition == NULL) {
        return -1;
    }

    uint32_t address = (lba * ESP_VISION_MSC_BLOCK_SIZE) + offset;
    if ((address + bufsize) > partition->size) {
        return -1;
    }

    if (esp_partition_read(partition, address, buffer, bufsize) != ESP_OK) {
        return -1;
    }

    return (int32_t)bufsize;
}

static uint32_t esp_vision_msc_min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static bool esp_vision_msc_write_chunk(const esp_partition_t *partition,
                                       uint32_t address,
                                       const uint8_t *buffer,
                                       uint32_t len)
{
    while (len > 0) {
        uint32_t erase_base = (address / ESP_VISION_MSC_ERASE_BLOCK_SIZE) * ESP_VISION_MSC_ERASE_BLOCK_SIZE;
        uint32_t erase_offset = address - erase_base;
        uint32_t write_len = esp_vision_msc_min_u32(len, ESP_VISION_MSC_ERASE_BLOCK_SIZE - erase_offset);

        if ((erase_offset == 0) && (write_len == ESP_VISION_MSC_ERASE_BLOCK_SIZE)) {
            if (esp_partition_erase_range(partition, erase_base, ESP_VISION_MSC_ERASE_BLOCK_SIZE) != ESP_OK) {
                return false;
            }
            if (esp_partition_write(partition, erase_base, buffer, ESP_VISION_MSC_ERASE_BLOCK_SIZE) != ESP_OK) {
                return false;
            }
        } else {
            if (esp_partition_read(partition, erase_base, s_msc_erase_cache, ESP_VISION_MSC_ERASE_BLOCK_SIZE) != ESP_OK) {
                return false;
            }
            memcpy(s_msc_erase_cache + erase_offset, buffer, write_len);
            if (esp_partition_erase_range(partition, erase_base, ESP_VISION_MSC_ERASE_BLOCK_SIZE) != ESP_OK) {
                return false;
            }
            if (esp_partition_write(partition, erase_base, s_msc_erase_cache, ESP_VISION_MSC_ERASE_BLOCK_SIZE) != ESP_OK) {
                return false;
            }
        }

        address += write_len;
        buffer += write_len;
        len -= write_len;
    }

    return true;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;

    const esp_partition_t *partition = esp_vision_msc_get_partition();
    if (partition == NULL) {
        return -1;
    }

    uint32_t address = (lba * ESP_VISION_MSC_BLOCK_SIZE) + offset;
    if ((address + bufsize) > partition->size) {
        return -1;
    }

    return esp_vision_msc_write_chunk(partition, address, buffer, bufsize) ? (int32_t)bufsize : -1;
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

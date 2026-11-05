/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_VISION_TEST_PARTITION_H
#define ESP_VISION_TEST_PARTITION_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_FAT 0x81
typedef struct {
    uint32_t address;
    uint32_t size;
    uint8_t type;
    uint8_t subtype;
    char label[17];
} esp_partition_t;
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *buffer, size_t length);
#endif

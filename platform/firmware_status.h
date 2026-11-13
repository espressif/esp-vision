/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char boot_id[33];
    char application_sha256[65];
    char elf_sha256[65];
    char partition_table_sha256[65];
    char bootloader_sha256[65];
    char operation_id[33];
    uint32_t application_address;
    uint32_t application_capacity;
    uint32_t reset_reason;
    int32_t commit_result;
    bool crash_reset;
} esp_vision_firmware_status_t;

/* Read-only installation evidence; called only by the EV-MUX transport task.
 * image_size is the exact packaged BIN length, not the partition capacity.
 * Success requires confirmed startup, valid OTA state and a readable receipt.
 */
esp_err_t esp_vision_firmware_status(uint32_t image_size, esp_vision_firmware_status_t *status);

/* Bounded read-only inventory for the retained installer's acceptance query. */
esp_err_t esp_vision_firmware_inventory(esp_vision_firmware_status_t *status);
const char *esp_vision_firmware_boot_id(void);

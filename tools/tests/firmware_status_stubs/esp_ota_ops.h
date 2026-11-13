/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct {
    uint32_t address;
    uint32_t size;
    unsigned subtype;
    char label[17];
} esp_partition_t;
typedef int esp_ota_img_states_t;
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_OTA_IMG_PENDING_VERIFY 1
#define ESP_OTA_IMG_VALID 2
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);

/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
typedef struct {
    uint8_t app_elf_sha256[32];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);

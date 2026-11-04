/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESP_VISION_TEST_ESP_MAC_H
#define ESP_VISION_TEST_ESP_MAC_H

#include <stdint.h>
#include "esp_err.h"

typedef enum { ESP_MAC_EFUSE_FACTORY } esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t mac[6], esp_mac_type_t type);

#endif

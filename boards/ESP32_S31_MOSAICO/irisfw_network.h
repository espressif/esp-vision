/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_VISION_IRISFW_NETWORK_H
#define ESP_VISION_IRISFW_NETWORK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];
    char password[65];
    bool enabled;
    uint32_t revision;
} esp_vision_irisfw_network_config_t;

esp_err_t esp_vision_irisfw_network_load(esp_vision_irisfw_network_config_t *config);
esp_err_t esp_vision_irisfw_network_save(const esp_vision_irisfw_network_config_t *config);

#endif

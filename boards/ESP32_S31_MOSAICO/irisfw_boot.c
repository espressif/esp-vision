/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <string.h>

#include "irisfw_boot.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Mosaico Recovery ABI 2 uses this NVS record independently of Iris transport.
 * Keep the rest of sysmeta intact; initialization errors never permit erasure.
 */
esp_err_t esp_vision_irisfw_confirm_boot(void)
{
    static bool confirmed;
    if (confirmed) {
        return ESP_OK;
    }
    const esp_partition_t *app = esp_ota_get_running_partition();
    if (app == NULL || app->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
            app->address != 0x210000 || strcmp(app->label, "main_app") != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_ota_img_states_t state;
    esp_err_t ret = esp_ota_get_state_partition(app, &state);
    if (ret != ESP_OK) {
        return ret;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY && state != ESP_OTA_IMG_VALID) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = nvs_flash_init_partition("sysmeta");
    if (ret != ESP_OK) {
        return ret;
    }
    nvs_handle_t metadata;
    ret = nvs_open_from_partition("sysmeta", "iris_ota_demo", NVS_READWRITE, &metadata);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret == ESP_OK) {
        ret = nvs_set_u32(metadata, "last_good", app->address);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u32(metadata, "target", 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(metadata);
    }
    nvs_close(metadata);
    confirmed = ret == ESP_OK;
    return ret;
}

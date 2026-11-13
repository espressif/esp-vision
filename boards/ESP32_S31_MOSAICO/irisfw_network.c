/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "irisfw_network.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Vibe Mode owns sysmeta/shared. Its two public string keys are the credential
 * source of truth; the ev_* integers retain ESP-VISION's enable/revision state.
 * Never erase this partition: it also contains boot and recovery metadata.
 */
static bool shared_password_valid(const char *password)
{
    size_t length = strlen(password);
    // Vibe accepts an open network or an 8..63-byte passphrase. Do not impose
    // the EV-MUX provisioning API's printable-ASCII policy on shared records.
    if (length == 0 || (length >= 8 && length <= 63)) {
        return true;
    }
    if (length != 64) {
        return false;
    }
    return strspn(password, "0123456789abcdefABCDEF") == length;
}

static esp_err_t read_credentials(const char *space, const char *ssid_key, const char *password_key,
                                  esp_vision_irisfw_network_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition("sysmeta", space, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t ssid_size = sizeof(config->ssid);
    size_t password_size = sizeof(config->password);
    esp_err_t ssid_error = nvs_get_str(handle, ssid_key, config->ssid, &ssid_size);
    esp_err_t password_error = nvs_get_str(handle, password_key, config->password, &password_size);
    if (ssid_error == ESP_ERR_NVS_NOT_FOUND && password_error == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NVS_NOT_FOUND;
    } else if (ssid_error != ESP_OK || password_error != ESP_OK) {
        err = ssid_error != ESP_OK ? ssid_error : password_error;
        // A partial shared pair must not revive an older private credential.
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_ERR_INVALID_STATE;
        }
    } else if (ssid_size == 0 || ssid_size > sizeof(config->ssid) ||
               password_size == 0 || password_size > sizeof(config->password) ||
               config->ssid[ssid_size - 1] != '\0' || config->password[password_size - 1] != '\0' ||
               strlen(config->ssid) != ssid_size - 1 || strlen(config->password) != password_size - 1 ||
               (config->ssid[0] == '\0' && config->password[0] != '\0') ||
               !shared_password_valid(config->password)) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        config->enabled = config->ssid[0] != '\0';
        config->revision = config->enabled ? 1 : 0;
        if (strcmp(space, "shared") == 0) {
            uint8_t enabled;
            err = nvs_get_u8(handle, "ev_enabled", &enabled);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            } else if (err == ESP_OK) {
                if (enabled > 1 || (enabled && config->ssid[0] == '\0')) {
                    err = ESP_ERR_INVALID_STATE;
                } else {
                    config->enabled = enabled;
                }
            }
            if (err == ESP_OK) {
                err = nvs_get_u32(handle, "ev_revision", &config->revision);
                if (err == ESP_ERR_NVS_NOT_FOUND) {
                    err = ESP_OK;
                }
            }
        }
    }
    nvs_close(handle);
    return err;
}

esp_err_t esp_vision_irisfw_network_load(esp_vision_irisfw_network_config_t *config)
{
    memset(config, 0, sizeof(*config));
    esp_err_t err = nvs_flash_init_partition("sysmeta");
    if (err == ESP_OK) {
        err = read_credentials("shared", "wifi_ssid", "wifi_password", config);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            // Older Vibe Mode installations used sysmeta/wifi. Read it only
            // when both shared keys are absent; boot never migrates or writes.
            err = read_credentials("wifi", "ssid", "password", config);
        }
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            memset(config, 0, sizeof(*config));
            return ESP_OK;
        }
    }
    if (err != ESP_OK) {
        memset(config, 0, sizeof(*config));
    }
    return err;
}

esp_err_t esp_vision_irisfw_network_save(const esp_vision_irisfw_network_config_t *config)
{
    if (memchr(config->ssid, '\0', sizeof(config->ssid)) == NULL ||
            memchr(config->password, '\0', sizeof(config->password)) == NULL ||
            (config->ssid[0] == '\0' && (config->password[0] != '\0' || config->enabled)) ||
            !shared_password_valid(config->password)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_flash_init_partition("sysmeta");
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t handle;
    err = nvs_open_from_partition("sysmeta", "shared", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    // Forget persists an explicit empty pair, so neither firmware can fall
    // back to an obsolete sysmeta/wifi record after a successful forget.
    err = nvs_set_str(handle, "wifi_ssid", config->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "wifi_password", config->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "ev_enabled", config->enabled);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "ev_revision", config->revision);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

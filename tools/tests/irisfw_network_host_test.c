/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <string.h>

#include "irisfw_network.h"
#include "nvs.h"

typedef struct {
    const char *key;
    int type;
    char text[80];
    uint32_t integer;
} entry_t;

static entry_t entries[2][8];
static unsigned opened, closed, writes, legacy_opens, commits;
static int init_error, open_error, read_error, write_error, commit_error;

static entry_t *entry(nvs_handle_t handle, const char *key, bool create)
{
    assert(handle == 1 || handle == 2);
    for (unsigned i = 0; i < 8; i++) {
        entry_t *value = &entries[handle - 1][i];
        if (value->key && strcmp(value->key, key) == 0) {
            return value;
        }
        if (!value->key && create) {
            value->key = key;
            return value;
        }
    }
    return NULL;
}

static void string(nvs_handle_t handle, const char *key, const char *text)
{
    entry_t *value = entry(handle, key, true);
    assert(value && strlen(text) < sizeof(value->text));
    value->type = 1;
    strcpy(value->text, text);
}

esp_err_t nvs_flash_init_partition(const char *partition)
{
    assert(strcmp(partition, "sysmeta") == 0);
    return init_error;
}

esp_err_t nvs_open_from_partition(const char *partition, const char *space, int mode, nvs_handle_t *handle)
{
    assert(strcmp(partition, "sysmeta") == 0);
    assert(mode == NVS_READONLY || mode == NVS_READWRITE);
    assert(strcmp(space, "shared") == 0 || strcmp(space, "wifi") == 0);
    if (open_error) {
        return open_error;
    }
    *handle = strcmp(space, "shared") == 0 ? 1 : 2;
    if (*handle == 2) {
        assert(mode == NVS_READONLY);
        legacy_opens++;
    }
    opened++;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *size)
{
    if (read_error) {
        return read_error;
    }
    entry_t *value = entry(handle, key, false);
    if (!value) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (value->type != 1) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    size_t length = strlen(value->text) + 1;
    if (*size < length) {
        *size = length;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, value->text, length);
    *size = length;
    return ESP_OK;
}

static esp_err_t integer(nvs_handle_t handle, const char *key, int type, uint32_t *out)
{
    entry_t *value = entry(handle, key, false);
    if (!value) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (value->type != type) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    *out = value->integer;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    uint32_t value;
    esp_err_t err = integer(handle, key, 2, &value);
    if (err == ESP_OK) {
        *out = value;
    }
    return err;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    return integer(handle, key, 3, out);
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    assert(handle == 1);
    assert(strcmp(key, "wifi_ssid") == 0 || strcmp(key, "wifi_password") == 0);
    if (write_error) {
        return write_error;
    }
    writes++;
    string(handle, key, value);
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 1 && strcmp(key, "ev_enabled") == 0);
    entry_t *found = entry(handle, key, true);
    found->type = 2;
    found->integer = value;
    writes++;
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    assert(handle == 1 && strcmp(key, "ev_revision") == 0);
    entry_t *found = entry(handle, key, true);
    found->type = 3;
    found->integer = value;
    writes++;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    commits++;
    return commit_error;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1 || handle == 2);
    closed++;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    esp_vision_irisfw_network_config_t config;
    memset(&config, 0xa5, sizeof(config));
    string(2, "ssid", "older-network");
    string(2, "password", "older-test-password");
    if (strcmp(scenario, "legacy") == 0) {
        assert(esp_vision_irisfw_network_load(&config) == ESP_OK);
        assert(strcmp(config.ssid, "older-network") == 0 && config.enabled && writes == 0);
    } else if (strcmp(scenario, "absent") == 0) {
        memset(entries, 0, sizeof(entries));
        assert(esp_vision_irisfw_network_load(&config) == ESP_OK);
        assert(!config.ssid[0] && !config.password[0] && !config.enabled && !config.revision && !writes);
    } else if (strcmp(scenario, "shared") == 0) {
        string(1, "wifi_ssid", "shared-network");
        string(1, "wifi_password", "shared-test-password");
        assert(esp_vision_irisfw_network_load(&config) == ESP_OK);
        assert(strcmp(config.ssid, "shared-network") == 0 && config.enabled && config.revision == 1);
        assert(!legacy_opens && !writes);
        string(1, "wifi_ssid", "updated-in-vibe-mode");
        assert(esp_vision_irisfw_network_load(&config) == ESP_OK);
        assert(strcmp(config.ssid, "updated-in-vibe-mode") == 0 && !writes);
    } else if (strcmp(scenario, "partial-pair") == 0) {
        for (unsigned i = 0; i < 2; i++) {
            memset(entries[0], 0, sizeof(entries[0]));
            string(1, i ? "wifi_password" : "wifi_ssid", "partial-test");
            assert(esp_vision_irisfw_network_load(&config) == ESP_ERR_INVALID_STATE);
            assert(!config.ssid[0] && !config.password[0] && !legacy_opens && !writes);
        }
    } else if (strcmp(scenario, "bounds") == 0) {
        char ssid[34], password[66];
        memset(ssid, 's', 32); ssid[32] = 0;
        memset(password, 'a', 64); password[64] = 0;
        string(1, "wifi_ssid", ssid); string(1, "wifi_password", password);
        assert(esp_vision_irisfw_network_load(&config) == ESP_OK);
        assert(strlen(config.ssid) == 32 && strlen(config.password) == 64);
        password[64] = 'a'; password[65] = 0;
        string(1, "wifi_password", password);
        assert(esp_vision_irisfw_network_load(&config) == ESP_ERR_NVS_INVALID_LENGTH);
        assert(!config.ssid[0] && !config.password[0] && !legacy_opens);
        ssid[32] = 's'; ssid[33] = 0;
        string(1, "wifi_ssid", ssid);
        assert(esp_vision_irisfw_network_load(&config) == ESP_ERR_NVS_INVALID_LENGTH);
    } else if (strcmp(scenario, "password-format") == 0) {
        string(1, "wifi_ssid", "shared-network");
        const char *accepted[] = {"", "12345678", "\xc3\xa9" "123456"};
        for (unsigned i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
            string(1, "wifi_password", accepted[i]);
            assert(esp_vision_irisfw_network_load(&config) == ESP_OK);
            assert(strcmp(config.password, accepted[i]) == 0);
            assert(esp_vision_irisfw_network_save(&config) == ESP_OK);
        }
        char invalid_psk[65];
        memset(invalid_psk, 'g', 64); invalid_psk[64] = 0;
        const char *rejected[] = {"1234567", invalid_psk};
        for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
            string(1, "wifi_password", rejected[i]);
            assert(esp_vision_irisfw_network_load(&config) == ESP_ERR_INVALID_STATE);
            assert(!config.ssid[0] && !config.password[0]);
            strcpy(config.ssid, "shared-network");
            strcpy(config.password, rejected[i]);
            assert(esp_vision_irisfw_network_save(&config) == ESP_ERR_INVALID_ARG);
        }
        assert(!legacy_opens);
    } else if (strcmp(scenario, "persist") == 0 || strcmp(scenario, "forget") == 0) {
        memset(&config, 0, sizeof(config));
        strcpy(config.ssid, "shared-new-network"); strcpy(config.password, "new-test-password");
        config.enabled = false; config.revision = 42;
        string(1, "brightness", "90");
        assert(esp_vision_irisfw_network_save(&config) == ESP_OK);
        assert(commits == 1 && writes == 4);
        if (strcmp(scenario, "forget") == 0) {
            memset(&config, 0, sizeof(config)); config.revision = 43;
            assert(esp_vision_irisfw_network_save(&config) == ESP_OK);
        }
        assert(esp_vision_irisfw_network_load(&config) == ESP_OK);
        assert(!config.enabled && !legacy_opens);
        assert(strcmp(entry(1, "brightness", false)->text, "90") == 0);
        if (strcmp(scenario, "forget") == 0) {
            assert(!config.ssid[0] && !config.password[0] && config.revision == 43);
        } else {
            assert(strcmp(config.ssid, "shared-new-network") == 0 && config.revision == 42);
        }
    } else if (strcmp(scenario, "metadata") == 0) {
        string(1, "wifi_ssid", "open-network"); string(1, "wifi_password", "");
        assert(esp_vision_irisfw_network_load(&config) == ESP_OK && config.enabled);
        nvs_set_u8(1, "ev_enabled", 2);
        assert(esp_vision_irisfw_network_load(&config) == ESP_ERR_INVALID_STATE);
        assert(!config.password[0] && !config.ssid[0]);
        string(1, "ev_enabled", "wrong-type");
        assert(esp_vision_irisfw_network_load(&config) == ESP_ERR_NVS_TYPE_MISMATCH);
    } else if (strcmp(scenario, "errors") == 0) {
        init_error = ESP_FAIL;
        assert(esp_vision_irisfw_network_load(&config) == ESP_FAIL && !opened);
        assert(!config.ssid[0] && !config.password[0]);
        init_error = ESP_OK; open_error = ESP_FAIL;
        assert(esp_vision_irisfw_network_load(&config) == ESP_FAIL && !opened);
        open_error = ESP_OK; read_error = ESP_FAIL;
        assert(esp_vision_irisfw_network_load(&config) == ESP_FAIL && !legacy_opens);
        read_error = ESP_OK; write_error = ESP_FAIL;
        assert(esp_vision_irisfw_network_save(&config) == ESP_FAIL && !commits);
        write_error = ESP_OK; commit_error = ESP_FAIL;
        assert(esp_vision_irisfw_network_save(&config) == ESP_FAIL);
    } else {
        assert(0);
    }
    assert(opened == closed);
    return 0;
}

/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
#include "firmware_status.h"
#include "irisfw_boot.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *scenario;
static esp_partition_t app = { 0x210000, 0x700000, 0x10, "main_app" };
static esp_ota_img_states_t state = ESP_OTA_IMG_PENDING_VERIFY;
static unsigned writes, reads, closes, random_calls;
static uint8_t image[2051], table[4096], bootloader[0x6000];
static bool matches(const char *name)
{
    return strcmp(scenario, name) == 0;
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return matches("missing-app") ? NULL : &app;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *out)
{
    assert(partition == &app);
    *out = state;
    return matches("ota-read-error") ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    ++writes;
    if (matches("ota-confirm-error")) {
        return ESP_FAIL;
    }
    state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}
esp_err_t nvs_flash_init_partition(const char *partition)
{
    assert(strcmp(partition, "sysmeta") == 0);
    return matches("nvs-init-error") ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_open_from_partition(const char *partition, const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(partition, "sysmeta") == 0);
    if (strcmp(name, "iris_ota_demo") == 0) {
        assert(mode == NVS_READWRITE);
        *handle = 1;
    } else {
        assert(strcmp(name, "update") == 0 && mode == NVS_READONLY);
        if (matches("receipt-missing")) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
        *handle = 2;
    }
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    assert(handle == 1);
    assert((strcmp(key, "last_good") == 0 && value == app.address) ||
           (strcmp(key, "target") == 0 && value == 0));
    ++writes;
    return matches("nvs-write-error") ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    ++writes;
    return matches("nvs-commit-error") ? ESP_FAIL : ESP_OK;
}
void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1 || handle == 2);
    ++closes;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    assert(handle == 2 && strcmp(key, "last_result") == 0 && *size == 64);
    uint32_t record[16] = { 0x49535953, 2 };
    for (unsigned i = 8; i < 24; ++i) {
        ((uint8_t *)record)[i] = (uint8_t)i;
    }
    if (matches("receipt-magic")) {
        record[0] ^= 1;
    }
    if (matches("receipt-version")) {
        record[1] = 1;
    }
    if (matches("receipt-failure")) {
        record[6] = 259;
    }
    if (matches("receipt-size")) {
        *size = 63;
    }
    memcpy(out, record, *size);
    return matches("receipt-read-error") ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_flash_read(void *chip, void *out, uint32_t address, uint32_t size)
{
    assert(chip == NULL && size > 0 && size <= 1024);
    ++reads;
    if (address >= app.address) {
        assert(address + size <= app.address + sizeof(image));
        if (matches("image-read-error")) {
            return ESP_FAIL;
        }
        memcpy(out, image + address - app.address, size);
    } else if (address < 0x8000) {
        assert(address >= 0x2000 && address + size <= 0x8000);
        if (matches("bootloader-read-error")) {
            return ESP_FAIL;
        }
        memcpy(out, bootloader + address - 0x2000, size);
    } else {
        assert(address >= 0x8000 && address + size <= 0x9000);
        if (matches("table-read-error")) {
            return ESP_FAIL;
        }
        memcpy(out, table + address - 0x8000, size);
    }
    return ESP_OK;
}
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t description = { .app_elf_sha256 = { 0x12, 0x34 } };
    return &description;
}
void esp_fill_random(void *out, size_t size)
{
    ++random_calls;
    memset(out, 0xab, size);
}
esp_reset_reason_t esp_reset_reason(void)
{
    return matches("crash") ? ESP_RST_PANIC : ESP_RST_SW;
}

static void check_digest(const uint8_t *data, size_t size, const char *actual)
{
    uint8_t digest[32];
    unsigned length;
    char expected[65];
    assert(EVP_Digest(data, size, digest, &length, EVP_sha256(), NULL) == 1 && length == 32);
    for (unsigned i = 0; i < 32; ++i) {
        snprintf(expected + i * 2, 3, "%02x", digest[i]);
    }
    assert(strcmp(actual, expected) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    for (unsigned i = 0; i < sizeof(image); ++i) {
        image[i] = (uint8_t)(i * 17);
    }
    for (unsigned i = 0; i < sizeof(table); ++i) {
        table[i] = (uint8_t)(i * 31);
    }
    esp_vision_firmware_status_t status;
    assert(esp_vision_firmware_status(sizeof(image), &status) != ESP_OK);
    assert(reads == 0 && writes == 0);
    if (matches("bad-address")) {
        app.address = 0x10000;
    }
    esp_err_t confirmed = esp_vision_irisfw_confirm_boot();
    if (matches("missing-app") || matches("bad-address") || matches("ota-read-error") ||
            matches("ota-confirm-error") || matches("nvs-init-error") ||
            matches("nvs-write-error") || matches("nvs-commit-error")) {
        assert(confirmed != ESP_OK && !esp_vision_irisfw_boot_confirmed());
        assert(esp_vision_firmware_status(sizeof(image), &status) != ESP_OK && reads == 0);
        return 0;
    }
    assert(confirmed == ESP_OK && esp_vision_irisfw_boot_confirmed());
    unsigned write_count = writes;
    assert(esp_vision_irisfw_confirm_boot() == ESP_OK && writes == write_count);
    assert(esp_vision_firmware_status(0, &status) == ESP_ERR_INVALID_ARG && reads == 0);
    assert(esp_vision_firmware_status(app.size + 1, &status) == ESP_ERR_INVALID_ARG && reads == 0);
    assert(esp_vision_firmware_status(sizeof(image), NULL) == ESP_ERR_INVALID_ARG);
    if (matches("bad-capacity")) {
        app.size--;
    }
    if (matches("bad-ota-state")) {
        state = ESP_OTA_IMG_PENDING_VERIFY;
    }
    esp_err_t ret = esp_vision_firmware_status(sizeof(image), &status);
    assert(writes == write_count);
    if (matches("success") || matches("crash") || matches("receipt-failure")) {
        assert(ret == ESP_OK && reads == 31 && closes == 2);
        check_digest(image, sizeof(image), status.application_sha256);
        check_digest(table, sizeof(table), status.partition_table_sha256);
        check_digest(bootloader, sizeof(bootloader), status.bootloader_sha256);
        assert(strcmp(status.operation_id, "08090a0b0c0d0e0f1011121314151617") == 0);
        assert(strncmp(status.elf_sha256, "12340000", 8) == 0);
        assert(status.commit_result == (matches("receipt-failure") ? 259 : 0));
        assert(status.crash_reset == matches("crash"));
        assert(status.application_address == 0x210000 && status.application_capacity == 0x700000);
        assert(strcmp(status.boot_id, "abababababababababababababababab") == 0);
        assert(esp_vision_firmware_status(sizeof(image), &status) == ESP_OK && random_calls == 1);
    } else {
        assert(ret != ESP_OK);
    }
    return 0;
}

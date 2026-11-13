/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>

#include "firmware_status.h"
#include "irisfw_boot.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#define STATUS_HASH_BLOCK (1024)
#define STATUS_PARTITION_TABLE_SIZE (4096)

/* The retained installer owns this on-flash ABI 2 receipt. This reader does
 * not implement its wire protocol or write/repair any retained metadata.
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t operation_id[16];
    int32_t result;
    uint8_t reserved[36];
} installation_receipt_t;

_Static_assert(sizeof(installation_receipt_t) == 64, "installation receipt size");
_Static_assert(offsetof(installation_receipt_t, result) == 24, "installation result offset");

static void hex_string(const uint8_t *bytes, size_t size, char *output)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        output[i * 2] = digits[bytes[i] >> 4];
        output[i * 2 + 1] = digits[bytes[i] & 15];
    }
    output[size * 2] = '\0';
}

static esp_err_t flash_sha256(uint32_t address, uint32_t size, char output[65])
{
    uint8_t block[STATUS_HASH_BLOCK];
    uint8_t digest[32];
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        (void)psa_hash_abort(&hash);
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_OK;
    while (size > 0) {
        size_t count = size < sizeof(block) ? size : sizeof(block);
        ret = esp_flash_read(NULL, block, address, count);
        if (ret != ESP_OK) {
            break;
        }
        if (psa_hash_update(&hash, block, count) != PSA_SUCCESS) {
            ret = ESP_FAIL;
            break;
        }
        address += count;
        size -= count;
    }
    size_t digest_size = 0;
    if (ret == ESP_OK && (psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS ||
                          digest_size != sizeof(digest))) {
        ret = ESP_FAIL;
    }
    (void)psa_hash_abort(&hash);
    if (ret == ESP_OK) {
        hex_string(digest, sizeof(digest), output);
    }
    return ret;
}

const char *esp_vision_firmware_boot_id(void)
{
    /* Only the transport task calls this function; the token survives VM soft
     * resets, but a hardware reboot creates a different token. */
    static char boot_id[33];
    if (boot_id[0] == '\0') {
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        hex_string(random, sizeof(random), boot_id);
    }
    return boot_id;
}

esp_err_t esp_vision_firmware_inventory(esp_vision_firmware_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    const esp_partition_t *app = esp_ota_get_running_partition();
    if (app == NULL || app->address != 0x210000 || app->size != 0x700000 ||
            app->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 || strcmp(app->label, "main_app") != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_vision_irisfw_boot_confirmed()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_ota_img_states_t state;
    esp_err_t ret = esp_ota_get_state_partition(app, &state);
    if (ret != ESP_OK || state != ESP_OTA_IMG_VALID) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t metadata;
    ret = nvs_open_from_partition("sysmeta", "update", NVS_READONLY, &metadata);
    if (ret != ESP_OK) {
        return ret;
    }
    installation_receipt_t receipt;
    size_t receipt_size = sizeof(receipt);
    ret = nvs_get_blob(metadata, "last_result", &receipt, &receipt_size);
    nvs_close(metadata);
    if (ret != ESP_OK) {
        return ret;
    }
    if (receipt_size != sizeof(receipt) || receipt.magic != 0x49535953 || receipt.version != 2) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = flash_sha256(CONFIG_BOOTLOADER_OFFSET_IN_FLASH,
                       CONFIG_PARTITION_TABLE_OFFSET - CONFIG_BOOTLOADER_OFFSET_IN_FLASH,
                       status->bootloader_sha256);
    if (ret == ESP_OK) {
        ret = flash_sha256(CONFIG_PARTITION_TABLE_OFFSET, STATUS_PARTITION_TABLE_SIZE,
                           status->partition_table_sha256);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    const esp_app_desc_t *description = esp_app_get_description();
    hex_string(description->app_elf_sha256, sizeof(description->app_elf_sha256), status->elf_sha256);
    hex_string(receipt.operation_id, sizeof(receipt.operation_id), status->operation_id);
    memcpy(status->boot_id, esp_vision_firmware_boot_id(), sizeof(status->boot_id));
    status->application_address = app->address;
    status->application_capacity = app->size;
    status->commit_result = receipt.result;
    esp_reset_reason_t reason = esp_reset_reason();
    status->reset_reason = (uint32_t)reason;
    status->crash_reset = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                          reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
                          reason == ESP_RST_BROWNOUT || reason == ESP_RST_PWR_GLITCH ||
                          reason == ESP_RST_CPU_LOCKUP;
    return ESP_OK;
}

esp_err_t esp_vision_firmware_status(uint32_t image_size, esp_vision_firmware_status_t *status)
{
    if (image_size == 0 || image_size > 0x700000) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = esp_vision_firmware_inventory(status);
    if (ret == ESP_OK) {
        ret = flash_sha256(status->application_address, image_size, status->application_sha256);
    }
    return ret;
}

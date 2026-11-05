/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Link the actual provisioning implementation against NVS/FatFs fault fakes.
 * The fake mount result models the storage driver; this is not a FAT parser
 * or physical flash/power-loss qualification. */
#include "../../platform/storage_provision.c"

static esp_partition_t s_partition;
static storage_provision_record_t s_record;
static storage_provision_record_t s_staged;
static size_t s_record_size;
static bool s_namespace_missing;
static esp_err_t s_open_error;
static esp_err_t s_get_error;
static esp_err_t s_set_error;
static unsigned s_commit_count;
static unsigned s_fail_commit;
static unsigned s_format_count;
static unsigned s_mount_count;
static unsigned s_read_count;
static bool s_read_error;
static FRESULT s_mount_result;
static FRESULT s_format_result;
static bool s_remount_error;
static uint8_t *s_flash;
static uint8_t s_work[4096];
static FATFS s_fs;
static esp_vision_storage_status_t s_status;
static unsigned s_open_handles, s_admissions, s_finishes;
static bool s_writer_allowed, s_writer_finishes, s_testing_sd;

uint32_t esp_vision_storage_vfs_open_handle_count(esp_vision_storage_volume_t volume)
{
    assert(volume == (s_testing_sd ? ESP_VISION_STORAGE_VOLUME_SD : ESP_VISION_STORAGE_VOLUME_FLASH));
    return s_open_handles;
}

bool esp_vision_msc_writer_vfs_begin(esp_vision_storage_volume_t volume, bool writing)
{
    assert(volume == (s_testing_sd ? ESP_VISION_STORAGE_VOLUME_SD : ESP_VISION_STORAGE_VOLUME_FLASH) && writing);
    ++s_admissions;
    return s_writer_allowed;
}

bool esp_vision_msc_writer_vfs_finish(esp_vision_storage_volume_t volume)
{
    assert(volume == (s_testing_sd ? ESP_VISION_STORAGE_VOLUME_SD : ESP_VISION_STORAGE_VOLUME_FLASH));
    ++s_finishes;
    return s_writer_finishes;
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "injected error";
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "ev_storage") == 0);
    if (s_open_error != ESP_OK) {
        return s_open_error;
    }
    if (mode == NVS_READONLY && s_namespace_missing) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    assert(handle == 1 && strcmp(key, "flash") == 0);
    if (s_get_error != ESP_OK) {
        return s_get_error;
    }
    if (s_record_size == 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    assert(s_record_size <= *length);
    memcpy(value, &s_record, s_record_size);
    *length = s_record_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    assert(handle == 1 && strcmp(key, "flash") == 0 && length == sizeof(s_staged));
    if (s_set_error == ESP_OK) {
        memcpy(&s_staged, value, length);
    }
    return s_set_error;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    if (++s_commit_count == s_fail_commit) {
        return ESP_FAIL;
    }
    s_record = s_staged;
    s_record_size = sizeof(s_record);
    s_namespace_missing = false;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *buffer, size_t length)
{
    assert(partition == &s_partition);
    assert(offset <= partition->size && length <= partition->size - offset);
    ++s_read_count;
    if (s_read_error) {
        return ESP_FAIL;
    }
    memcpy(buffer, s_flash + offset, length);
    return ESP_OK;
}

FRESULT f_mount(FATFS *filesystem, const char *drive, BYTE immediate)
{
    assert(filesystem == &s_fs && strcmp(drive, "0:") == 0 && immediate == 1);
    ++s_mount_count;
    return s_mount_count > 1 && s_remount_error ? FR_DISK_ERR : s_mount_result;
}

FRESULT f_mkfs(const char *drive, const MKFS_PARM *options, void *work, UINT size)
{
    assert(strcmp(drive, "0:") == 0 && work == s_work && size == sizeof(s_work));
    assert(options->fmt == ((s_testing_sd ? FM_ANY : FM_FAT) | FM_SFD) && options->n_fat == 2 && options->au_size == 4096);
    // Formatting must never begin until PENDING is durable and partition-bound.
    if (!s_testing_sd) {
        assert(s_record_size == sizeof(s_record) && s_record.state == STORAGE_PROVISION_PENDING);
        assert(s_record.address == s_partition.address && s_record.size == s_partition.size);
    }
    ++s_format_count;
    s_flash[0] = 0xeb; // A partial format may already look mountable after reboot.
    if (s_format_result == FR_OK) {
        s_mount_result = FR_OK;
    }
    return s_format_result;
}

static esp_err_t boot(bool confirmed)
{
    s_mount_count = 0;
    return esp_vision_storage_mount_flash_fat(&s_partition, &s_fs, "0:", s_work, sizeof(s_work), confirmed, false, &s_status);
}

static esp_err_t automatic_mount(bool enabled)
{
    s_mount_count = 0;
    return s_testing_sd ? esp_vision_storage_mount_sd_fat(&s_fs, "0:", s_work, sizeof(s_work), enabled, &s_status) :
           esp_vision_storage_mount_flash_fat(&s_partition, &s_fs, "0:", s_work, sizeof(s_work), false, enabled, &s_status);
}

static void reset(void)
{
    s_partition = (esp_partition_t) {
        .address = 0x710000, .size = 0x8f0000,
        .type = ESP_PARTITION_TYPE_DATA, .subtype = ESP_PARTITION_SUBTYPE_DATA_FAT, .label = "ffat",
    };
    memset(s_flash, 0xff, s_partition.size);
    memset(&s_record, 0, sizeof(s_record));
    s_record_size = 0;
    s_namespace_missing = false;
    s_open_error = s_get_error = s_set_error = ESP_OK;
    s_commit_count = s_fail_commit = s_format_count = s_mount_count = s_read_count = 0;
    s_read_error = s_remount_error = false;
    s_mount_result = FR_NO_FILESYSTEM;
    s_format_result = FR_OK;
    s_open_handles = s_admissions = s_finishes = 0;
    s_writer_allowed = s_writer_finishes = true;
    s_testing_sd = false;
}

static void test_mount_failure_never_authorizes_writes(void)
{
    reset();
    assert(boot(false) == ESP_ERR_INVALID_STATE);
    assert(s_status.state == ESP_VISION_STORAGE_BLANK && !s_status.mounted);
    assert(s_format_count == 0 && s_commit_count == 0);
    s_namespace_missing = true;
    s_flash[0x100000] = 0x42; // Korvo factory resource at physical 0x810000.
    s_flash[s_partition.size - 1] = 0x7f;
    assert(boot(false) == ESP_ERR_INVALID_STATE);
    assert(s_status.state == ESP_VISION_STORAGE_UNRECOGNIZED && s_status.fatfs_result == FR_NO_FILESYSTEM);
    assert(s_format_count == 0 && s_commit_count == 0 && s_flash[0x100000] == 0x42);
    assert(boot(false) == ESP_ERR_INVALID_STATE); // Retry does not silently initialize.
    assert(s_format_count == 0 && s_commit_count == 0);
}

static void test_explicit_confirmation_and_existing_files(void)
{
    reset();
    s_flash[0x100000] = 0x42;
    assert(boot(true) == ESP_OK && s_format_count == 1 && s_commit_count == 2);
    assert(s_status.state == ESP_VISION_STORAGE_READY && s_status.mounted);
    s_flash[8192] = 0x23;
    assert(boot(false) == ESP_OK && s_format_count == 1 && s_commit_count == 2);
    assert(s_flash[8192] == 0x23);
    assert(boot(true) == ESP_ERR_INVALID_STATE && s_format_count == 1); // Readable FAT is never erased.
    s_mount_result = FR_NO_FILESYSTEM;
    assert(boot(false) == ESP_ERR_INVALID_STATE && s_format_count == 1 && s_flash[8192] == 0x23);
    memset(s_flash, 0xff, s_partition.size);
    assert(boot(false) == ESP_ERR_INVALID_STATE && s_format_count == 1); // Even blank recovery needs a decision.
    assert(boot(true) == ESP_OK && s_format_count == 2);
}

static void test_legacy_fat_is_adopted_without_writes(void)
{
    reset();
    s_mount_result = FR_OK;
    s_flash[8192] = 0x23;
    assert(boot(false) == ESP_OK && s_format_count == 0 && s_commit_count == 0 && s_flash[8192] == 0x23);
    s_mount_result = FR_NO_FILESYSTEM;
    assert(boot(false) == ESP_ERR_INVALID_STATE && s_format_count == 0 && s_flash[8192] == 0x23);
    reset();
    strcpy(s_partition.label, "vfs");
    assert(boot(false) == ESP_ERR_INVALID_STATE && !s_status.format_supported);
    assert(boot(true) == ESP_ERR_NOT_SUPPORTED && s_format_count == 0 && s_commit_count == 0);
    s_mount_result = FR_OK;
    assert(boot(false) == ESP_OK && s_commit_count == 0);
    reset();
    s_partition.subtype = 0x83;
    assert(boot(true) == ESP_ERR_NOT_SUPPORTED && s_format_count == 0);
    reset();
    s_partition.type = 0;
    assert(boot(true) == ESP_ERR_NOT_SUPPORTED && s_format_count == 0);
}

static void assert_interrupted_requires_a_new_decision(void)
{
    assert(s_record.state == STORAGE_PROVISION_PENDING);
    unsigned formats = s_format_count, commits = s_commit_count;
    s_mount_result = FR_OK; // A partial FAT header cannot prove mkfs completion.
    s_remount_error = false;
    s_format_result = FR_OK;
    assert(boot(false) == ESP_ERR_INVALID_STATE && s_status.state == ESP_VISION_STORAGE_FORMAT_INTERRUPTED);
    assert(s_mount_count == 0 && s_format_count == formats && s_commit_count == commits);
    assert(boot(true) == ESP_OK && s_format_count == formats + 1 && s_record.state == STORAGE_PROVISION_READY);
}

static void test_interruption_never_replays_a_destructive_operation(void)
{
    reset();
    s_format_result = FR_DISK_ERR;
    assert(boot(true) == ESP_FAIL);
    assert_interrupted_requires_a_new_decision();
    reset();
    s_remount_error = true;
    assert(boot(true) == ESP_FAIL);
    assert_interrupted_requires_a_new_decision();
    reset();
    s_fail_commit = 2;
    assert(boot(true) == ESP_FAIL);
    assert_interrupted_requires_a_new_decision();
}

static void test_nvs_failures_prevent_destructive_writes(void)
{
    reset();
    s_open_error = ESP_FAIL;
    assert(boot(true) == ESP_FAIL && s_format_count == 0 && s_status.state == ESP_VISION_STORAGE_STATE_ERROR);
    reset();
    s_get_error = ESP_FAIL;
    assert(boot(true) == ESP_FAIL && s_format_count == 0);
    reset();
    s_set_error = ESP_FAIL;
    assert(boot(true) == ESP_FAIL && s_format_count == 0);
    reset();
    s_fail_commit = 1;
    assert(boot(true) == ESP_FAIL && s_format_count == 0);
    reset();
    assert(boot(true) == ESP_OK);
    s_record_size--;
    assert(boot(false) == ESP_ERR_INVALID_STATE && s_format_count == 1);
    assert(boot(true) == ESP_ERR_INVALID_STATE && s_format_count == 1);
    s_record_size = sizeof(s_record);
    s_record.magic = 0;
    assert(boot(true) == ESP_ERR_INVALID_STATE && s_format_count == 1);
    s_record.magic = STORAGE_RECORD_MAGIC;
    s_record.version = 99;
    assert(boot(true) == ESP_ERR_INVALID_STATE && s_format_count == 1);
    s_record.version = STORAGE_RECORD_VERSION;
    s_record.state = 99;
    assert(boot(true) == ESP_ERR_INVALID_STATE && s_format_count == 1);
}

static void test_changed_layout_and_io_errors_preserve_data(void)
{
    reset();
    s_format_result = FR_DISK_ERR;
    assert(boot(true) == ESP_FAIL);
    s_record.address += 4096;
    assert(boot(false) == ESP_ERR_INVALID_STATE && s_status.state == ESP_VISION_STORAGE_FORMAT_INTERRUPTED);
    s_record.size -= 4096;
    assert(boot(false) == ESP_ERR_INVALID_STATE && s_format_count == 1);
    const FRESULT errors[] = {FR_DISK_ERR, FR_NOT_READY};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        reset();
        s_mount_result = errors[i];
        assert(boot(false) == ESP_FAIL && s_status.state == ESP_VISION_STORAGE_IO_ERROR);
        assert(boot(true) == ESP_FAIL && s_format_count == 0 && s_commit_count == 0);
    }
    reset();
    s_mount_result = FR_INT_ERR;
    assert(boot(false) == ESP_ERR_INVALID_STATE && s_format_count == 0);
    assert(boot(true) == ESP_OK && s_format_count == 1);
    reset();
    s_read_error = true;
    assert(boot(false) == ESP_FAIL && s_status.state == ESP_VISION_STORAGE_IO_ERROR && s_format_count == 0);
    reset();
    assert(esp_vision_storage_mount_flash_fat(NULL, &s_fs, "0:", s_work, sizeof(s_work), false, false, &s_status) == ESP_ERR_INVALID_ARG);
    assert(esp_vision_storage_mount_flash_fat(&s_partition, &s_fs, "0:", s_work, 512, true, false, &s_status) == ESP_ERR_INVALID_ARG);
    assert(s_mount_count == 0);
}

static void test_automatic_format_policy(void)
{
    for (unsigned sd = 0; sd < 2; ++sd) {
        reset();
        s_testing_sd = sd;
        s_flash[0x100000] = 0x42; // Nonblank, unrecognized factory/foreign contents.
        assert(automatic_mount(false) != ESP_OK && s_format_count == 0 && s_admissions == 0);
        assert(automatic_mount(true) == ESP_OK && s_status.mounted && s_format_count == 1);
        assert(s_admissions == 1 && s_finishes == 1);
        assert(automatic_mount(true) == ESP_OK && s_format_count == 1);
        s_mount_result = FR_NO_FILESYSTEM;
        assert(automatic_mount(true) == ESP_OK && s_format_count == 2); // Policy also covers previously used media.
        const FRESULT failures[] = {FR_OK, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY};
        for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
            reset();
            s_testing_sd = sd;
            s_mount_result = failures[i];
            automatic_mount(true);
            assert(s_format_count == 0 && s_admissions == 0 && s_commit_count == 0);
        }
        reset();
        s_testing_sd = sd;
        s_open_handles = 1;
        assert(automatic_mount(true) != ESP_OK && s_format_count == 0 && s_admissions == 0);
        s_open_handles = 0;
        s_writer_allowed = false;
        assert(automatic_mount(true) != ESP_OK && s_format_count == 0 && s_finishes == 0);
        s_writer_allowed = true;
        s_format_result = FR_DISK_ERR;
        assert(automatic_mount(true) != ESP_OK && !s_status.mounted && s_finishes == 1);
        if (!sd) {
            unsigned formats = s_format_count;
            assert(automatic_mount(true) != ESP_OK && s_format_count == formats); // PENDING is not policy consent.
        }
        reset();
        s_testing_sd = sd;
        s_writer_finishes = false;
        assert(automatic_mount(true) != ESP_OK && !s_status.mounted && s_finishes == 1);
    }
    reset();
    strcpy(s_partition.label, "vfs");
    assert(automatic_mount(true) != ESP_OK && s_format_count == 0);
    reset();
    s_fail_commit = 1;
    assert(automatic_mount(true) != ESP_OK && s_format_count == 0 && s_finishes == 1);
}

int main(void)
{
    s_flash = malloc(0x8f0000);
    assert(s_flash != NULL);
    test_mount_failure_never_authorizes_writes();
    test_explicit_confirmation_and_existing_files();
    test_legacy_fat_is_adopted_without_writes();
    test_interruption_never_replays_a_destructive_operation();
    test_nvs_failures_prevent_destructive_writes();
    test_changed_layout_and_io_errors_preserve_data();
    test_automatic_format_policy();
    free(s_flash);
    return 0;
}

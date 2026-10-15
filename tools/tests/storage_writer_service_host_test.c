/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * This host harness links the production writer service and USB MSC callback
 * adapter against narrow NVS, FreeRTOS, FAT, and block-device fakes. It does
 * not model USB hardware or claim a cross-host MSC exclusion qualification.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "freertos/task.h"
#include "storage.h"
#include "storage_writer.h"

/* Compile the implementation into this translation unit so the harness can
 * observe its private gate at deterministic fault-injection boundaries. */
#include "../../platform/storage_writer_msc.c"
#include "../../platform/usb_msc.c"

#define TEST_RECORD_MAX 128

static uint8_t s_nvs_blob[ESP_VISION_STORAGE_VOLUME_COUNT][TEST_RECORD_MAX];
static size_t s_nvs_blob_length[ESP_VISION_STORAGE_VOLUME_COUNT];
static uint32_t s_open_handles[ESP_VISION_STORAGE_VOLUME_COUNT];
static unsigned int s_delay_count;
static bool s_finish_vfs_on_first_delay;
static bool s_close_handle_on_second_delay;
static bool s_complete_write_on_first_delay;
static unsigned int s_flash_sync_count;
static unsigned int s_sd_sync_count;
static unsigned int s_refresh_count;
static esp_err_t s_flash_write_result = ESP_OK;
static bool s_quarantine_during_commit;

static int volume_for_key(const char *key)
{
    if (strcmp(key, "flash") == 0) {
        return ESP_VISION_STORAGE_VOLUME_FLASH;
    }
    if (strcmp(key, "sd") == 0) {
        return ESP_VISION_STORAGE_VOLUME_SD;
    }
    return -1;
}

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "ev_wgate") == 0);
    (void)mode;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    assert(handle == 1);
    int volume = volume_for_key(key);
    assert(volume >= 0);
    if (s_nvs_blob_length[volume] == 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*length < s_nvs_blob_length[volume]) {
        *length = s_nvs_blob_length[volume];
        return ESP_FAIL;
    }
    memcpy(out_value, s_nvs_blob[volume], s_nvs_blob_length[volume]);
    *length = s_nvs_blob_length[volume];
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    assert(handle == 1);
    int volume = volume_for_key(key);
    assert(volume >= 0);
    assert(length <= TEST_RECORD_MAX);
    memcpy(s_nvs_blob[volume], value, length);
    s_nvs_blob_length[volume] = length;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    if (s_quarantine_during_commit) {
        s_quarantine_during_commit = false;
        /* Model a concurrent session invalidation arriving while the old
         * transition's NVS write is delayed. The writer must reject that
         * stale transition and let quarantine become the durable last state. */
        portENTER_CRITICAL(&s_writer_mux);
        esp_vision_storage_writer_quarantine(&s_gate[ESP_VISION_STORAGE_VOLUME_FLASH]);
        portEXIT_CRITICAL(&s_writer_mux);
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
}

void esp_fill_random(void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    for (size_t index = 0; index < length; index++) {
        bytes[index] = (uint8_t)(0xA0U + index);
    }
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    s_delay_count++;
    if (s_delay_count == 1 && s_finish_vfs_on_first_delay) {
        assert(!esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, false));
        bool finished = esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH);
        assert(finished);
        s_finish_vfs_on_first_delay = false;
    }
    if (s_delay_count == 1 && s_complete_write_on_first_delay) {
        uint32_t sequence = 0;
        assert(esp_vision_msc_writer_host_write_sync_begin(0, &sequence));
        assert(sequence != 0);
        assert(esp_vision_msc_writer_host_write_complete(0, sequence, true));
        s_complete_write_on_first_delay = false;
    }
    if (s_delay_count == 2 && s_close_handle_on_second_delay) {
        assert(s_open_handles[ESP_VISION_STORAGE_VOLUME_FLASH] == 1);
        s_open_handles[ESP_VISION_STORAGE_VOLUME_FLASH]--;
        s_close_handle_on_second_delay = false;
    }
}

esp_err_t esp_vision_storage_flash_sync(void)
{
    s_flash_sync_count++;
    return ESP_OK;
}

esp_err_t esp_vision_storage_sdcard_sync(void)
{
    s_sd_sync_count++;
    return ESP_OK;
}

uint32_t esp_vision_storage_flash_block_count(void)
{
    return 1024;
}

esp_err_t esp_vision_storage_flash_read(uint32_t address, void *buffer, size_t len)
{
    (void)address;
    memset(buffer, 0, len);
    return ESP_OK;
}

esp_err_t esp_vision_storage_flash_write(uint32_t address, const void *buffer, size_t len)
{
    (void)address;
    (void)buffer;
    (void)len;
    return s_flash_write_result;
}

bool esp_vision_storage_sdcard_is_ready(void)
{
    return true;
}

uint32_t esp_vision_storage_sdcard_block_count(void)
{
    return 1024;
}

esp_err_t esp_vision_storage_sdcard_read(uint64_t address, void *buffer, size_t len)
{
    (void)address;
    memset(buffer, 0, len);
    return ESP_OK;
}

esp_err_t esp_vision_storage_sdcard_write(uint64_t address, const void *buffer, size_t len)
{
    (void)address;
    (void)buffer;
    (void)len;
    return ESP_OK;
}

void tud_msc_set_sense(uint8_t lun,
                       uint8_t sense_key,
                       uint8_t additional_sense_code,
                       uint8_t additional_sense_code_qualifier)
{
    (void)lun;
    (void)sense_key;
    (void)additional_sense_code;
    (void)additional_sense_code_qualifier;
}

esp_err_t esp_vision_storage_refresh_volume(esp_vision_storage_volume_t volume)
{
    assert((unsigned int)volume < ESP_VISION_STORAGE_VOLUME_COUNT);
    s_refresh_count++;
    return ESP_OK;
}

uint32_t esp_vision_storage_vfs_open_handle_count(esp_vision_storage_volume_t volume)
{
    assert((unsigned int)volume < ESP_VISION_STORAGE_VOLUME_COUNT);
    return s_open_handles[volume];
}

static void setup(void)
{
    assert(nvs_flash_init() == ESP_OK);
    assert(esp_vision_msc_writer_init() == ESP_OK);
    assert(esp_vision_msc_writer_host_ready(0));
    /* Fresh/legacy devices retain their prior application VFS behavior. This
     * unqualified compatibility mode is not the managed single-writer path. */
    assert(esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, true));
    assert(esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH));
}

static void test_real_service_vfs_transition_drains_open_call_and_handle(void)
{
    setup();
    esp_vision_storage_writer_status_t status;
    assert(esp_vision_msc_writer_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status) == ESP_OK);
    assert(status.writable && status.mode == ESP_VISION_STORAGE_WRITER_AVAILABLE);

    /* The production hook admits before the VFS syscall. The handle becomes
     * visible before finish drops in-flight, so the transition must wait for
     * both milestones before syncing/remounting. */
    assert(esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, false));
    s_open_handles[ESP_VISION_STORAGE_VOLUME_FLASH] = 1;
    s_delay_count = 0;
    s_finish_vfs_on_first_delay = true;
    s_close_handle_on_second_delay = true;
    assert(esp_vision_msc_writer_host_start_stop(0, false, true));
    assert(s_delay_count >= 2);
    assert(s_flash_sync_count == 1);
    assert(s_refresh_count == 1);
    assert(!esp_vision_msc_writer_host_ready(0));
    assert(esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, false));
    assert(esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH));
    assert(esp_vision_msc_writer_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status) == ESP_OK);
    assert(status.mode == ESP_VISION_STORAGE_WRITER_HANDOFF_READY && !status.writable);

    char lease_id[33];
    const char *operation = "0123456789abcdef0123456789abcdef";
    const char *session = "abcdef0123456789abcdef0123456789";
    assert(esp_vision_msc_writer_acquire(ESP_VISION_STORAGE_VOLUME_FLASH,
                                         operation,
                                         session,
                                         lease_id) == ESP_OK);
    assert(strlen(lease_id) == 32);
    assert(esp_vision_msc_writer_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status) == ESP_OK);
    assert(status.mode == ESP_VISION_STORAGE_WRITER_HELD && status.writable);
    assert(strcmp(status.operation_id, operation) == 0);
    assert(strcmp(status.session_id, session) == 0);
    assert(strcmp(status.lease_id, lease_id) == 0);

    assert(esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, true));
    assert(esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH));
    assert(esp_vision_msc_writer_release(ESP_VISION_STORAGE_VOLUME_FLASH,
                                         operation,
                                         lease_id,
                                         session,
                                         true) == ESP_OK);
    assert(esp_vision_msc_writer_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status) == ESP_OK);
    assert(status.mode == ESP_VISION_STORAGE_WRITER_HANDOFF_READY && !status.writable);
    assert(esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, false));
    assert(esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, true));
    assert(esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH));
    assert(esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH));

    /* A released managed handoff is reusable for a later Agent-owned
     * operation; it must not require a USB LOAD between every transaction. */
    char second_lease_id[33];
    const char *second_operation = "11111111111111111111111111111111";
    assert(esp_vision_msc_writer_acquire(ESP_VISION_STORAGE_VOLUME_FLASH,
                                         second_operation,
                                         session,
                                         second_lease_id) == ESP_OK);
    assert(esp_vision_msc_writer_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status) == ESP_OK);
    assert(status.mode == ESP_VISION_STORAGE_WRITER_HELD && status.writable);
    assert(esp_vision_msc_writer_release(ESP_VISION_STORAGE_VOLUME_FLASH,
                                         second_operation,
                                         second_lease_id,
                                         session,
                                         true) == ESP_OK);

    assert(esp_vision_msc_writer_host_start_stop(0, true, true));
    assert(esp_vision_msc_writer_host_ready(0));
    assert(!esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, true));

    /* Managed host ownership survives reboot; a later host eject hands the
     * volume back to the device, and a subsequent LOAD closes VFS admission. */
    s_writer_initialized = false;
    s_writer_store_ready = false;
    assert(esp_vision_msc_writer_init() == ESP_OK);
    assert(esp_vision_msc_writer_host_ready(0));
    assert(!esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, false));
    assert(esp_vision_msc_writer_host_start_stop(0, false, true));
    assert(!esp_vision_msc_writer_host_ready(0));
    assert(esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, true));
    assert(esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH));
    assert(esp_vision_msc_writer_host_start_stop(0, true, true));
    assert(esp_vision_msc_writer_host_ready(0));
    assert(!esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, false));

    assert(esp_vision_msc_writer_host_io_begin(0));
    assert(esp_vision_msc_writer_host_io_finish(0, true));
    s_delay_count = 0;
    s_complete_write_on_first_delay = true;
    assert(esp_vision_msc_writer_host_start_stop(0, false, true));
    assert(s_flash_sync_count == 9);
}

static void test_legacy_eject_load_reboot_keeps_legacy_vfs_access(void)
{
    setup();
    assert(!s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].managed);
    assert(esp_vision_msc_writer_host_start_stop(0, false, true));
    assert(s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].handoff_ready);
    assert(!s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].managed);
    assert(esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, true));
    assert(esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH));

    assert(esp_vision_msc_writer_host_start_stop(0, true, true));
    assert(esp_vision_msc_writer_host_ready(0));
    assert(!s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].managed);
    assert(esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, false));
    assert(esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH));

    s_writer_initialized = false;
    s_writer_store_ready = false;
    assert(esp_vision_msc_writer_init() == ESP_OK);
    assert(esp_vision_msc_writer_host_ready(0));
    assert(!s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].managed);
    assert(esp_vision_msc_writer_vfs_begin(ESP_VISION_STORAGE_VOLUME_FLASH, true));
    assert(esp_vision_msc_writer_vfs_finish(ESP_VISION_STORAGE_VOLUME_FLASH));
}

static void test_production_usb_callback_syncs_one_multichunk_write_command(void)
{
    setup();
    uint8_t chunk[512] = { 0 };

    /* TinyUSB may invoke WRITE10 once per chunk, then calls complete only once
     * for the SCSI command. The actual production callback must sync and clear
     * all three accepted chunks on that single completion. */
    for (uint32_t chunk_index = 0; chunk_index < 3; chunk_index++) {
        assert(tud_msc_write10_cb(0, chunk_index, 0, chunk, sizeof(chunk)) == (int32_t)sizeof(chunk));
    }
    assert(s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].pending_write_sync == 3);
    assert(s_flash_sync_count == 0);
    tud_msc_write10_complete_cb(0);
    assert(s_flash_sync_count == 1);
    assert(s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].pending_write_sync == 0);
    assert(!s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].quarantined);
    assert(esp_vision_msc_writer_host_start_stop(0, false, true));
}

static void test_production_usb_empty_completion_is_noop(void)
{
    setup();
    /* A completion callback for a zero-data/failed command has no write
     * sequence: it must neither sync nor underflow/quarantine the volume. */
    tud_msc_write10_complete_cb(0);
    assert(s_flash_sync_count == 0);
    assert(!s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].quarantined);
    assert(esp_vision_msc_writer_host_ready(0));

    assert(esp_vision_msc_writer_host_start_stop(0, false, true));
    uint8_t chunk[512] = { 0 };
    assert(tud_msc_write10_cb(0, 0, 0, chunk, sizeof(chunk)) == -1);
    tud_msc_write10_complete_cb(0);
    assert(!s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].quarantined);
    assert(s_gate[ESP_VISION_STORAGE_VOLUME_FLASH].handoff_ready);
    assert(s_flash_sync_count == 1); /* only the eject sync */
}

static void test_actual_host_write_sync_failure_quarantines(void)
{
    setup();
    assert(esp_vision_msc_writer_host_io_begin(0));
    assert(esp_vision_msc_writer_host_io_finish(0, true));
    uint32_t sequence = 0;
    assert(esp_vision_msc_writer_host_write_sync_begin(0, &sequence));
    assert(sequence == 1);
    assert(!esp_vision_msc_writer_host_write_complete(0, sequence, false));
    assert(!esp_vision_msc_writer_host_ready(0));
    esp_vision_storage_writer_status_t status;
    assert(esp_vision_msc_writer_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status) == ESP_OK);
    assert(status.mode == ESP_VISION_STORAGE_WRITER_QUARANTINED && !status.writable);
}

static void test_late_transition_store_cannot_overwrite_quarantine(void)
{
    setup();
    assert(esp_vision_msc_writer_host_start_stop(0, false, true));
    const char *operation = "0123456789abcdef0123456789abcdef";
    const char *session = "abcdef0123456789abcdef0123456789";
    char lease_id[33];
    assert(esp_vision_msc_writer_acquire(ESP_VISION_STORAGE_VOLUME_FLASH,
                                         operation,
                                         session,
                                         lease_id) == ESP_OK);

    s_quarantine_during_commit = true;
    assert(esp_vision_msc_writer_release(ESP_VISION_STORAGE_VOLUME_FLASH,
                                         operation,
                                         lease_id,
                                         session,
                                         true) != ESP_OK);
    esp_vision_storage_writer_status_t status;
    assert(esp_vision_msc_writer_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status) == ESP_OK);
    assert(status.mode == ESP_VISION_STORAGE_WRITER_QUARANTINED);
    assert(!esp_vision_msc_writer_host_ready(0));

    writer_record_t persisted = { 0 };
    assert(s_nvs_blob_length[ESP_VISION_STORAGE_VOLUME_FLASH] == sizeof(persisted));
    memcpy(&persisted, s_nvs_blob[ESP_VISION_STORAGE_VOLUME_FLASH], sizeof(persisted));
    assert(writer_record_valid(&persisted));
    assert(persisted.state == WRITER_PERSIST_QUARANTINE);

    s_writer_initialized = false;
    s_writer_store_ready = false;
    assert(esp_vision_msc_writer_init() == ESP_OK);
    assert(esp_vision_msc_writer_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status) == ESP_OK);
    assert(status.mode == ESP_VISION_STORAGE_WRITER_QUARANTINED);
    assert(!esp_vision_msc_writer_host_ready(0));
}

static void test_actual_session_change_quarantines_lease(void)
{
    setup();
    assert(esp_vision_msc_writer_host_start_stop(0, false, true));
    char lease_id[33];
    assert(esp_vision_msc_writer_acquire(ESP_VISION_STORAGE_VOLUME_FLASH,
                                         "0123456789abcdef0123456789abcdef",
                                         "abcdef0123456789abcdef0123456789",
                                         lease_id) == ESP_OK);
    esp_vision_msc_writer_session_changed("11111111111111111111111111111111");
    assert(!esp_vision_msc_writer_host_ready(0));
    esp_vision_storage_writer_status_t status;
    assert(esp_vision_msc_writer_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status) == ESP_OK);
    assert(status.mode == ESP_VISION_STORAGE_WRITER_QUARANTINED && !status.writable);
}

static void test_open_handle_timeout_never_refreshes_volume(void)
{
    setup();
    s_open_handles[ESP_VISION_STORAGE_VOLUME_FLASH] = 1;
    assert(!esp_vision_msc_writer_host_start_stop(0, false, true));
    assert(s_refresh_count == 0);
    assert(!esp_vision_msc_writer_host_ready(0));
    esp_vision_storage_writer_status_t status;
    assert(esp_vision_msc_writer_status(ESP_VISION_STORAGE_VOLUME_FLASH, &status) == ESP_OK);
    assert(status.mode == ESP_VISION_STORAGE_WRITER_QUARANTINED);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "transition") == 0) {
        test_real_service_vfs_transition_drains_open_call_and_handle();
    } else if (strcmp(argv[1], "legacy-eject-load-reboot") == 0) {
        test_legacy_eject_load_reboot_keeps_legacy_vfs_access();
    } else if (strcmp(argv[1], "usb-multichunk") == 0) {
        test_production_usb_callback_syncs_one_multichunk_write_command();
    } else if (strcmp(argv[1], "usb-empty-complete") == 0) {
        test_production_usb_empty_completion_is_noop();
    } else if (strcmp(argv[1], "host-write-failure") == 0) {
        test_actual_host_write_sync_failure_quarantines();
    } else if (strcmp(argv[1], "stale-transition-quarantine") == 0) {
        test_late_transition_store_cannot_overwrite_quarantine();
    } else if (strcmp(argv[1], "session-change") == 0) {
        test_actual_session_change_quarantines_lease();
    } else if (strcmp(argv[1], "open-timeout") == 0) {
        test_open_handle_timeout_never_refreshes_volume();
    } else {
        assert(!"unknown test scenario");
    }
    return 0;
}

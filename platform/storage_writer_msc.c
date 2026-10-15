/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage_writer.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "py/mpconfig.h"
#include "soc/soc_caps.h"
#include "storage.h"
#include "storage_writer_gate.h"
#include "tusb.h"

#if MICROPY_HW_ENABLE_USBDEV && MICROPY_HW_USB_MSC

#define WRITER_NAMESPACE "ev_wgate"
#define WRITER_RECORD_MAGIC UINT32_C(0x45565747)
#define WRITER_RECORD_VERSION 1
#define WRITER_DRAIN_TIMEOUT_MS 2000U
#define WRITER_DRAIN_POLL_MS 10U

typedef enum {
    WRITER_PERSIST_HOST = 1,
    WRITER_PERSIST_HANDOFF = 2,
    WRITER_PERSIST_LEASED = 3,
    WRITER_PERSIST_QUARANTINE = 4,
    WRITER_PERSIST_MANAGED_HOST = 5,
} writer_persist_state_t;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t state;
    uint8_t reserved[2];
    char operation_id[33];
    char lease_id[33];
    char session_id[33];
} writer_record_t;

static portMUX_TYPE s_writer_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_writer_store_lock_buffer;
static SemaphoreHandle_t s_writer_store_lock;
static esp_vision_storage_writer_gate_t s_gate[ESP_VISION_STORAGE_VOLUME_COUNT] = {
    { .ejected = true, .quarantined = true },
    { .ejected = true, .quarantined = true },
};
static writer_record_t s_record[ESP_VISION_STORAGE_VOLUME_COUNT];
static bool s_writer_initialized;
static bool s_writer_store_ready;

static bool writer_store_state_allowed(esp_vision_storage_volume_t volume,
                                       writer_persist_state_t state)
{
    bool allowed;
    portENTER_CRITICAL(&s_writer_mux);
    const esp_vision_storage_writer_gate_t gate = s_gate[volume];
    if (state == WRITER_PERSIST_QUARANTINE) {
        allowed = gate.quarantined;
    } else if (gate.quarantined) {
        allowed = false;
    } else {
        switch (state) {
        case WRITER_PERSIST_HOST:
            allowed = !gate.managed && !gate.transition_pending && !gate.lease_held;
            break;
        case WRITER_PERSIST_HANDOFF:
        case WRITER_PERSIST_MANAGED_HOST:
            allowed = gate.managed && gate.transition_pending;
            break;
        case WRITER_PERSIST_LEASED:
            allowed = gate.transition_pending && !gate.lease_held;
            break;
        default:
            allowed = false;
            break;
        }
    }
    portEXIT_CRITICAL(&s_writer_mux);
    return allowed;
}

static const char *writer_nvs_key(esp_vision_storage_volume_t volume)
{
    return volume == ESP_VISION_STORAGE_VOLUME_FLASH ? "flash" : "sd";
}

static bool writer_volume_compiled(esp_vision_storage_volume_t volume)
{
    if (volume == ESP_VISION_STORAGE_VOLUME_FLASH) {
        return true;
    }
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    return volume == ESP_VISION_STORAGE_VOLUME_SD;
#else
    (void)volume;
    return false;
#endif
}

static bool writer_hex_id(const char *value)
{
    if (value == NULL || strlen(value) != 32) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static void writer_hex_encode(const uint8_t *input, size_t length, char *output)
{
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) {
        output[i * 2] = HEX[input[i] >> 4];
        output[i * 2 + 1] = HEX[input[i] & 0x0f];
    }
    output[length * 2] = '\0';
}

static bool writer_record_valid(const writer_record_t *record)
{
    if (record->magic != WRITER_RECORD_MAGIC || record->version != WRITER_RECORD_VERSION) {
        return false;
    }
    if (memchr(record->operation_id, '\0', sizeof(record->operation_id)) == NULL ||
            memchr(record->lease_id, '\0', sizeof(record->lease_id)) == NULL ||
            memchr(record->session_id, '\0', sizeof(record->session_id)) == NULL) {
        return false;
    }
    switch ((writer_persist_state_t)record->state) {
    case WRITER_PERSIST_HOST:
    case WRITER_PERSIST_HANDOFF:
    case WRITER_PERSIST_QUARANTINE:
    case WRITER_PERSIST_MANAGED_HOST:
        return record->operation_id[0] == '\0' && record->lease_id[0] == '\0' && record->session_id[0] == '\0';
    case WRITER_PERSIST_LEASED:
        return writer_hex_id(record->operation_id) && writer_hex_id(record->lease_id) && writer_hex_id(record->session_id);
    default:
        return false;
    }
}

static esp_err_t writer_store(esp_vision_storage_volume_t volume,
                              writer_persist_state_t state,
                              const char *operation_id,
                              const char *lease_id,
                              const char *session_id)
{
    if (!s_writer_store_ready || !writer_volume_compiled(volume) || s_writer_store_lock == NULL ||
            xSemaphoreTake(s_writer_store_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!writer_store_state_allowed(volume, state)) {
        xSemaphoreGive(s_writer_store_lock);
        return ESP_ERR_INVALID_STATE;
    }
    writer_record_t record = {
        .magic = WRITER_RECORD_MAGIC,
        .version = WRITER_RECORD_VERSION,
        .state = (uint8_t)state,
    };
    if (state == WRITER_PERSIST_LEASED) {
        if (!writer_hex_id(operation_id) || !writer_hex_id(lease_id) || !writer_hex_id(session_id)) {
            xSemaphoreGive(s_writer_store_lock);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(record.operation_id, operation_id, sizeof(record.operation_id));
        memcpy(record.lease_id, lease_id, sizeof(record.lease_id));
        memcpy(record.session_id, session_id, sizeof(record.session_id));
    }
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WRITER_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_writer_store_lock);
        return ret;
    }
    ret = nvs_set_blob(handle, writer_nvs_key(volume), &record, sizeof(record));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_writer_mux);
        bool still_allowed;
        if (state == WRITER_PERSIST_QUARANTINE) {
            still_allowed = s_gate[volume].quarantined;
        } else {
            still_allowed = !s_gate[volume].quarantined &&
                            s_gate[volume].transition_pending &&
                            ((state == WRITER_PERSIST_LEASED) || s_gate[volume].managed);
        }
        if (still_allowed) {
            s_record[volume] = record;
        } else {
            ret = ESP_ERR_INVALID_STATE;
        }
        portEXIT_CRITICAL(&s_writer_mux);
    }
    xSemaphoreGive(s_writer_store_lock);
    return ret;
}

static esp_err_t writer_load_record(esp_vision_storage_volume_t volume)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WRITER_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_record[volume], 0, sizeof(s_record[volume]));
        s_record[volume].magic = WRITER_RECORD_MAGIC;
        s_record[volume].version = WRITER_RECORD_VERSION;
        s_record[volume].state = WRITER_PERSIST_HOST;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    writer_record_t record = { 0 };
    size_t length = sizeof(record);
    ret = nvs_get_blob(handle, writer_nvs_key(volume), &record, &length);
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_record[volume], 0, sizeof(s_record[volume]));
        s_record[volume].magic = WRITER_RECORD_MAGIC;
        s_record[volume].version = WRITER_RECORD_VERSION;
        s_record[volume].state = WRITER_PERSIST_HOST;
        return ESP_OK;
    }
    if (ret != ESP_OK || length != sizeof(record) || !writer_record_valid(&record)) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    s_record[volume] = record;
    return ESP_OK;
}

static bool writer_gate_quiescent(esp_vision_storage_volume_t volume)
{
    esp_vision_storage_writer_gate_t gate;
    portENTER_CRITICAL(&s_writer_mux);
    gate = s_gate[volume];
    portEXIT_CRITICAL(&s_writer_mux);
    uint32_t open_handles = esp_vision_storage_vfs_open_handle_count(volume);
    return esp_vision_storage_writer_quiescent(&gate, open_handles);
}

static bool writer_gate_open_handles(esp_vision_storage_volume_t volume)
{
    return esp_vision_storage_vfs_open_handle_count(volume) == 0;
}

static bool writer_wait_quiescent(esp_vision_storage_volume_t volume)
{
    TickType_t waited = 0;
    const TickType_t poll = pdMS_TO_TICKS(WRITER_DRAIN_POLL_MS);
    const TickType_t deadline = pdMS_TO_TICKS(WRITER_DRAIN_TIMEOUT_MS);
    while (!writer_gate_quiescent(volume)) {
        if (waited >= deadline) {
            return false;
        }
        vTaskDelay(poll == 0 ? 1 : poll);
        waited += poll == 0 ? 1 : poll;
    }
    return true;
}

static void writer_quarantine(esp_vision_storage_volume_t volume)
{
    portENTER_CRITICAL(&s_writer_mux);
    esp_vision_storage_writer_quarantine(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    (void)writer_store(volume, WRITER_PERSIST_QUARANTINE, NULL, NULL, NULL);
}

static esp_err_t writer_refresh(esp_vision_storage_volume_t volume)
{
    if (!writer_gate_open_handles(volume)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = volume == ESP_VISION_STORAGE_VOLUME_FLASH
                    ? esp_vision_storage_flash_sync()
                    : esp_vision_storage_sdcard_sync();
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_vision_storage_refresh_volume(volume);
}

static bool writer_lun_volume(uint8_t lun, esp_vision_storage_volume_t *volume)
{
    if (lun == 0) {
        *volume = ESP_VISION_STORAGE_VOLUME_FLASH;
        return true;
    }
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    if (lun == 1) {
        *volume = ESP_VISION_STORAGE_VOLUME_SD;
        return true;
    }
#endif
    return false;
}

esp_err_t esp_vision_msc_writer_init(void)
{
    if (s_writer_initialized) {
        return s_writer_store_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (s_writer_store_lock == NULL) {
        s_writer_store_lock = xSemaphoreCreateMutexStatic(&s_writer_store_lock_buffer);
    }
    if (s_writer_store_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_writer_initialized = true;
    /* boardctrl_startup initializes NVS first. Do not initialize it twice or
     * erase/recreate it here; unreadable state remains quarantined below. */
    s_writer_store_ready = true;
    for (unsigned int i = 0; i < ESP_VISION_STORAGE_VOLUME_COUNT; i++) {
        esp_vision_storage_volume_t volume = (esp_vision_storage_volume_t)i;
        if (!writer_volume_compiled(volume)) {
            esp_vision_storage_writer_gate_init(&s_gate[volume], true, false);
            continue;
        }
        esp_err_t ret = writer_load_record(volume);
        if (ret != ESP_OK) {
            esp_vision_storage_writer_gate_init(&s_gate[volume], true, false);
            s_record[volume].state = WRITER_PERSIST_QUARANTINE;
            s_record[volume].magic = WRITER_RECORD_MAGIC;
            s_record[volume].version = WRITER_RECORD_VERSION;
            continue;
        }
        switch ((writer_persist_state_t)s_record[volume].state) {
        case WRITER_PERSIST_HOST:
            esp_vision_storage_writer_gate_init(&s_gate[volume], false, false);
            break;
        case WRITER_PERSIST_MANAGED_HOST:
            esp_vision_storage_writer_gate_init(&s_gate[volume], false, false);
            s_gate[volume].managed = true;
            break;
        case WRITER_PERSIST_HANDOFF:
            esp_vision_storage_writer_gate_init(&s_gate[volume], false, true);
            break;
        case WRITER_PERSIST_LEASED:
        case WRITER_PERSIST_QUARANTINE:
        default:
            // An interrupted lease is never replayed after reboot. Host MSC
            // remains ejected until a separately verified recovery path exists.
            esp_vision_storage_writer_gate_init(&s_gate[volume], true, false);
            break;
        }
    }
    return ESP_OK;
}

esp_err_t esp_vision_msc_writer_status(esp_vision_storage_volume_t volume,
                                       esp_vision_storage_writer_status_t *out)
{
    if (out == NULL || (unsigned int)volume >= ESP_VISION_STORAGE_VOLUME_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->mode = ESP_VISION_STORAGE_WRITER_UNKNOWN;
    out->reason = "WRITER_STATE_UNKNOWN";
    if (!s_writer_initialized || !s_writer_store_ready || !writer_volume_compiled(volume)) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_writer_mux);
    const esp_vision_storage_writer_gate_t gate = s_gate[volume];
    const writer_record_t record = s_record[volume];
    portEXIT_CRITICAL(&s_writer_mux);
    if (gate.quarantined) {
        out->mode = ESP_VISION_STORAGE_WRITER_QUARANTINED;
        out->reason = "WRITER_HANDOFF_UNCERTAIN";
    } else if (gate.lease_held) {
        out->mode = ESP_VISION_STORAGE_WRITER_HELD;
        out->writable = true;
        memcpy(out->operation_id, record.operation_id, sizeof(out->operation_id));
        memcpy(out->lease_id, record.lease_id, sizeof(out->lease_id));
        memcpy(out->session_id, record.session_id, sizeof(out->session_id));
        out->reason = NULL;
    } else if (gate.handoff_ready) {
        out->mode = ESP_VISION_STORAGE_WRITER_HANDOFF_READY;
        out->reason = NULL;
    } else if (gate.ejected) {
        out->mode = ESP_VISION_STORAGE_WRITER_AVAILABLE;
        out->reason = NULL;
    } else {
        out->mode = ESP_VISION_STORAGE_WRITER_AVAILABLE;
        out->writable = true;
        out->reason = "HOST_WRITER_ACTIVE";
    }
    return ESP_OK;
}

esp_err_t esp_vision_msc_writer_acquire(esp_vision_storage_volume_t volume,
                                        const char *operation_id,
                                        const char *session_id,
                                        char out_lease_id[33])
{
    if ((unsigned int)volume >= ESP_VISION_STORAGE_VOLUME_COUNT || !writer_volume_compiled(volume) ||
            !writer_hex_id(operation_id) || !writer_hex_id(session_id) || out_lease_id == NULL ||
            !s_writer_initialized || !s_writer_store_ready) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool started = esp_vision_storage_writer_begin_acquire(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    char lease_id[33];
    writer_hex_encode(random, sizeof(random), lease_id);
    if (!writer_wait_quiescent(volume) || writer_refresh(volume) != ESP_OK ||
            writer_store(volume, WRITER_PERSIST_LEASED, operation_id, lease_id, session_id) != ESP_OK) {
        writer_quarantine(volume);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool finished = esp_vision_storage_writer_finish_acquire(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    if (!finished) {
        writer_quarantine(volume);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(out_lease_id, lease_id, sizeof(lease_id));
    return ESP_OK;
}

esp_err_t esp_vision_msc_writer_release(esp_vision_storage_volume_t volume,
                                        const char *operation_id,
                                        const char *lease_id,
                                        const char *session_id,
                                        bool safe_handoff)
{
    if ((unsigned int)volume >= ESP_VISION_STORAGE_VOLUME_COUNT || !writer_volume_compiled(volume) ||
            !safe_handoff || !writer_hex_id(operation_id) || !writer_hex_id(lease_id) || !writer_hex_id(session_id) ||
            !s_writer_initialized || !s_writer_store_ready) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool matches = s_gate[volume].lease_held && !s_gate[volume].quarantined &&
                   strcmp(s_record[volume].operation_id, operation_id) == 0 &&
                   strcmp(s_record[volume].lease_id, lease_id) == 0 &&
                   strcmp(s_record[volume].session_id, session_id) == 0;
    bool started = matches && esp_vision_storage_writer_begin_release(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    if (!started || !writer_wait_quiescent(volume) || writer_refresh(volume) != ESP_OK ||
            writer_store(volume, WRITER_PERSIST_HANDOFF, NULL, NULL, NULL) != ESP_OK) {
        writer_quarantine(volume);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool finished = esp_vision_storage_writer_finish_release(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    if (!finished) {
        writer_quarantine(volume);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void esp_vision_msc_writer_session_changed(const char *current_session_id)
{
    if (!s_writer_initialized || !s_writer_store_ready) {
        return;
    }
    for (unsigned int i = 0; i < ESP_VISION_STORAGE_VOLUME_COUNT; i++) {
        esp_vision_storage_volume_t volume = (esp_vision_storage_volume_t)i;
        if (!writer_volume_compiled(volume)) {
            continue;
        }
        portENTER_CRITICAL(&s_writer_mux);
        bool invalidate = (s_gate[volume].lease_held || s_gate[volume].transition_pending) &&
                          (current_session_id == NULL || strcmp(current_session_id, s_record[volume].session_id) != 0);
        if (invalidate) {
            esp_vision_storage_writer_session_invalidated(&s_gate[volume]);
        }
        portEXIT_CRITICAL(&s_writer_mux);
        if (invalidate) {
            (void)writer_store(volume, WRITER_PERSIST_QUARANTINE, NULL, NULL, NULL);
        }
    }
}

bool esp_vision_msc_writer_host_io_begin(uint8_t lun)
{
    esp_vision_storage_volume_t volume;
    if (!writer_lun_volume(lun, &volume) || !s_writer_initialized || !s_writer_store_ready) {
        return false;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool allowed = esp_vision_storage_writer_begin_host_io(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    return allowed;
}

bool esp_vision_msc_writer_host_io_finish(uint8_t lun, bool accepted_write)
{
    esp_vision_storage_volume_t volume;
    if (!writer_lun_volume(lun, &volume)) {
        return false;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool ok = esp_vision_storage_writer_finish_host_io(&s_gate[volume], accepted_write);
    portEXIT_CRITICAL(&s_writer_mux);
    if (!ok) {
        writer_quarantine(volume);
    }
    return ok;
}

bool esp_vision_msc_writer_host_write_sync_begin(uint8_t lun, uint32_t *out_sequence)
{
    esp_vision_storage_volume_t volume;
    if (!writer_lun_volume(lun, &volume) || out_sequence == NULL ||
            !s_writer_initialized || !s_writer_store_ready) {
        return false;
    }
    portENTER_CRITICAL(&s_writer_mux);
    *out_sequence = s_gate[volume].pending_write_sync;
    bool valid = !s_gate[volume].quarantined;
    portEXIT_CRITICAL(&s_writer_mux);
    return valid;
}

bool esp_vision_msc_writer_host_write_complete(uint8_t lun,
                                               uint32_t sync_sequence,
                                               bool sync_succeeded)
{
    esp_vision_storage_volume_t volume;
    if (!writer_lun_volume(lun, &volume)) {
        return false;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool ok = esp_vision_storage_writer_complete_host_write(&s_gate[volume],
                                                            sync_sequence,
                                                            sync_succeeded);
    portEXIT_CRITICAL(&s_writer_mux);
    if (!ok && sync_sequence != 0) {
        writer_quarantine(volume);
    }
    return ok;
}

void esp_vision_msc_writer_host_io_failed(uint8_t lun)
{
    esp_vision_storage_volume_t volume;
    if (writer_lun_volume(lun, &volume)) {
        writer_quarantine(volume);
    }
}

bool esp_vision_msc_writer_host_ready(uint8_t lun)
{
    esp_vision_storage_volume_t volume;
    if (!writer_lun_volume(lun, &volume) || !s_writer_initialized || !s_writer_store_ready) {
        return false;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool ready = esp_vision_storage_writer_host_writable(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    return ready;
}

bool esp_vision_msc_writer_host_writable(uint8_t lun)
{
    return esp_vision_msc_writer_host_ready(lun);
}

bool esp_vision_msc_writer_python_io_allowed(esp_vision_storage_volume_t volume)
{
    if ((unsigned int)volume >= ESP_VISION_STORAGE_VOLUME_COUNT || !writer_volume_compiled(volume) ||
            !s_writer_initialized || !s_writer_store_ready) {
        return false;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool allowed = esp_vision_storage_writer_python_io_allowed(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    return allowed;
}

bool esp_vision_msc_writer_python_write_allowed(esp_vision_storage_volume_t volume)
{
    return esp_vision_msc_writer_python_io_allowed(volume);
}

bool esp_vision_msc_writer_vfs_begin(esp_vision_storage_volume_t volume, bool writing)
{
    if ((unsigned int)volume >= ESP_VISION_STORAGE_VOLUME_COUNT || !writer_volume_compiled(volume) ||
            !s_writer_initialized || !s_writer_store_ready) {
        return false;
    }
    (void)writing; // Read and write VFS calls share the same transition/drain gate.
    portENTER_CRITICAL(&s_writer_mux);
    bool allowed = esp_vision_storage_writer_begin_vfs_io(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    return allowed;
}

bool esp_vision_msc_writer_vfs_finish(esp_vision_storage_volume_t volume)
{
    if ((unsigned int)volume >= ESP_VISION_STORAGE_VOLUME_COUNT || !writer_volume_compiled(volume) ||
            !s_writer_initialized || !s_writer_store_ready) {
        return false;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool finished = esp_vision_storage_writer_finish_vfs_io(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    if (!finished) {
        writer_quarantine(volume);
    }
    return finished;
}

static void writer_host_quarantine(esp_vision_storage_volume_t volume)
{
    writer_quarantine(volume);
}

bool esp_vision_msc_writer_host_start_stop(uint8_t lun, bool start, bool load_eject)
{
    esp_vision_storage_volume_t volume;
    if (!writer_lun_volume(lun, &volume) || !s_writer_initialized || !s_writer_store_ready) {
        return false;
    }
    if (!load_eject) {
        if (start) {
            return true;
        }
        return volume == ESP_VISION_STORAGE_VOLUME_FLASH
               ? esp_vision_storage_flash_sync() == ESP_OK
               : esp_vision_storage_sdcard_sync() == ESP_OK;
    }
    if (!start) {
        portENTER_CRITICAL(&s_writer_mux);
        bool started = esp_vision_storage_writer_begin_eject(&s_gate[volume]);
        bool managed = s_gate[volume].managed;
        portEXIT_CRITICAL(&s_writer_mux);
        if (!started || !writer_wait_quiescent(volume) || writer_refresh(volume) != ESP_OK ||
                (managed && writer_store(volume, WRITER_PERSIST_HANDOFF, NULL, NULL, NULL) != ESP_OK)) {
            writer_host_quarantine(volume);
            return false;
        }
        portENTER_CRITICAL(&s_writer_mux);
        bool finished = esp_vision_storage_writer_finish_eject(&s_gate[volume]);
        portEXIT_CRITICAL(&s_writer_mux);
        if (!finished) {
            writer_host_quarantine(volume);
            return false;
        }
        return true;
    }

    portENTER_CRITICAL(&s_writer_mux);
    bool already_host = esp_vision_storage_writer_host_writable(&s_gate[volume]);
    bool managed = s_gate[volume].managed;
    bool allowed = esp_vision_storage_writer_begin_load(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    if (already_host) {
        return true;
    }
    if (!allowed || !writer_wait_quiescent(volume) || writer_refresh(volume) != ESP_OK ||
            (managed && writer_store(volume, WRITER_PERSIST_MANAGED_HOST, NULL, NULL, NULL) != ESP_OK)) {
        writer_host_quarantine(volume);
        return false;
    }
    portENTER_CRITICAL(&s_writer_mux);
    bool finished = esp_vision_storage_writer_finish_load(&s_gate[volume]);
    portEXIT_CRITICAL(&s_writer_mux);
    if (!finished) {
        writer_host_quarantine(volume);
        return false;
    }
    return true;
}

#else

esp_err_t esp_vision_msc_writer_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_vision_msc_writer_status(esp_vision_storage_volume_t volume, esp_vision_storage_writer_status_t *out)
{
    (void)volume;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_vision_msc_writer_acquire(esp_vision_storage_volume_t volume, const char *operation_id, const char *session_id, char out_lease_id[33])
{
    (void)volume;
    (void)operation_id;
    (void)session_id;
    (void)out_lease_id;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_vision_msc_writer_release(esp_vision_storage_volume_t volume, const char *operation_id, const char *lease_id, const char *session_id, bool safe_handoff)
{
    (void)volume;
    (void)operation_id;
    (void)lease_id;
    (void)session_id;
    (void)safe_handoff;
    return ESP_ERR_NOT_SUPPORTED;
}
void esp_vision_msc_writer_session_changed(const char *current_session_id)
{
    (void)current_session_id;
}
bool esp_vision_msc_writer_host_io_begin(uint8_t lun)
{
    (void)lun;
    return false;
}
bool esp_vision_msc_writer_host_io_finish(uint8_t lun, bool accepted_write)
{
    (void)lun;
    (void)accepted_write;
    return false;
}
bool esp_vision_msc_writer_host_write_sync_begin(uint8_t lun, uint32_t *out_sequence)
{
    (void)lun;
    (void)out_sequence;
    return false;
}
bool esp_vision_msc_writer_host_write_complete(uint8_t lun, uint32_t sync_sequence, bool sync_succeeded)
{
    (void)lun;
    (void)sync_sequence;
    (void)sync_succeeded;
    return false;
}
void esp_vision_msc_writer_host_io_failed(uint8_t lun)
{
    (void)lun;
}
bool esp_vision_msc_writer_host_ready(uint8_t lun)
{
    (void)lun;
    return false;
}
bool esp_vision_msc_writer_host_writable(uint8_t lun)
{
    (void)lun;
    return false;
}
bool esp_vision_msc_writer_python_io_allowed(esp_vision_storage_volume_t volume)
{
    (void)volume;
    return true;
}
bool esp_vision_msc_writer_python_write_allowed(esp_vision_storage_volume_t volume)
{
    (void)volume;
    return true;
}
bool esp_vision_msc_writer_vfs_begin(esp_vision_storage_volume_t volume, bool writing)
{
    (void)volume;
    (void)writing;
    return true;
}
bool esp_vision_msc_writer_vfs_finish(esp_vision_storage_volume_t volume)
{
    (void)volume;
    return true;
}
bool esp_vision_msc_writer_host_start_stop(uint8_t lun, bool start, bool load_eject)
{
    (void)lun;
    (void)start;
    (void)load_eject;
    return false;
}

#endif

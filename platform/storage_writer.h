/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_STORAGE_WRITER_H
#define ESP_VISION_STORAGE_WRITER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ESP_VISION_STORAGE_VOLUME_FLASH = 0,
    ESP_VISION_STORAGE_VOLUME_SD = 1,
    ESP_VISION_STORAGE_VOLUME_COUNT = 2,
} esp_vision_storage_volume_t;

typedef enum {
    ESP_VISION_STORAGE_WRITER_UNKNOWN = 0,
    ESP_VISION_STORAGE_WRITER_AVAILABLE,
    ESP_VISION_STORAGE_WRITER_HELD,
    ESP_VISION_STORAGE_WRITER_HANDOFF_READY,
    ESP_VISION_STORAGE_WRITER_QUARANTINED,
} esp_vision_storage_writer_mode_t;

typedef struct {
    esp_vision_storage_writer_mode_t mode;
    bool writable;
    char operation_id[33];
    char lease_id[33];
    char session_id[33];
    const char *reason;
} esp_vision_storage_writer_status_t;

esp_err_t esp_vision_msc_writer_init(void);
esp_err_t esp_vision_msc_writer_status(esp_vision_storage_volume_t volume,
                                       esp_vision_storage_writer_status_t *out);
esp_err_t esp_vision_msc_writer_acquire(esp_vision_storage_volume_t volume,
                                        const char *operation_id,
                                        const char *session_id,
                                        char out_lease_id[33]);
esp_err_t esp_vision_msc_writer_release(esp_vision_storage_volume_t volume,
                                        const char *operation_id,
                                        const char *lease_id,
                                        const char *session_id,
                                        bool safe_handoff);
void esp_vision_msc_writer_session_changed(const char *current_session_id);

/* Called around raw MSC block callbacks. Each successful WRITE10 data chunk
 * advances a pending sequence; TinyUSB snapshots that sequence before its
 * once-per-command completion sync and only clears writes covered by it. */
bool esp_vision_msc_writer_host_io_begin(uint8_t lun);
bool esp_vision_msc_writer_host_io_finish(uint8_t lun, bool accepted_write);
/* Snapshot the accepted-write sequence before a block-device sync. A zero
 * sequence means this SCSI command produced no successful write chunks. */
bool esp_vision_msc_writer_host_write_sync_begin(uint8_t lun, uint32_t *out_sequence);
bool esp_vision_msc_writer_host_write_complete(uint8_t lun,
                                               uint32_t sync_sequence,
                                               bool sync_succeeded);
void esp_vision_msc_writer_host_io_failed(uint8_t lun);
bool esp_vision_msc_writer_host_ready(uint8_t lun);
bool esp_vision_msc_writer_host_writable(uint8_t lun);
bool esp_vision_msc_writer_host_start_stop(uint8_t lun, bool start, bool load_eject);
bool esp_vision_msc_writer_python_io_allowed(esp_vision_storage_volume_t volume);
bool esp_vision_msc_writer_python_write_allowed(esp_vision_storage_volume_t volume);
/* Atomically admits a complete MicroPython VFS operation into the same drain
 * counter used by MSC. A transition first closes admission under the same
 * lock, then waits for this operation and all open handles to drain. */
bool esp_vision_msc_writer_vfs_begin(esp_vision_storage_volume_t volume, bool writing);
bool esp_vision_msc_writer_vfs_finish(esp_vision_storage_volume_t volume);

#endif /* ESP_VISION_STORAGE_WRITER_H */

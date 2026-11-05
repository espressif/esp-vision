/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_STORAGE_STATUS_H
#define ESP_VISION_STORAGE_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ESP_VISION_STORAGE_UNAVAILABLE = 0,
    ESP_VISION_STORAGE_READY,
    ESP_VISION_STORAGE_ABSENT,
    ESP_VISION_STORAGE_UNSUPPORTED,
    ESP_VISION_STORAGE_UNRECOGNIZED,
    ESP_VISION_STORAGE_BLANK,
    ESP_VISION_STORAGE_IO_ERROR,
    ESP_VISION_STORAGE_FORMAT_INTERRUPTED,
    ESP_VISION_STORAGE_STATE_ERROR,
    ESP_VISION_STORAGE_FORMATTING,
    ESP_VISION_STORAGE_UNMOUNTED,
} esp_vision_storage_state_t;

typedef struct {
    esp_vision_storage_state_t state;
    esp_err_t error;
    int fatfs_result;
    uint32_t address;
    uint32_t size;
    bool supported;
    bool present;
    bool mounted;
    bool format_supported;
} esp_vision_storage_status_t;

static inline const char *esp_vision_storage_state_name(esp_vision_storage_state_t state)
{
    switch (state) {
    case ESP_VISION_STORAGE_READY: return "ready";
    case ESP_VISION_STORAGE_ABSENT: return "absent";
    case ESP_VISION_STORAGE_UNSUPPORTED: return "unsupported";
    case ESP_VISION_STORAGE_UNRECOGNIZED: return "unrecognized";
    case ESP_VISION_STORAGE_BLANK: return "blank";
    case ESP_VISION_STORAGE_IO_ERROR: return "io_error";
    case ESP_VISION_STORAGE_FORMAT_INTERRUPTED: return "format_interrupted";
    case ESP_VISION_STORAGE_STATE_ERROR: return "state_error";
    case ESP_VISION_STORAGE_FORMATTING: return "formatting";
    case ESP_VISION_STORAGE_UNMOUNTED: return "unmounted";
    default: return "unavailable";
    }
}

static inline const char *esp_vision_storage_status_code(esp_vision_storage_state_t state)
{
    switch (state) {
    case ESP_VISION_STORAGE_READY: return "OK";
    case ESP_VISION_STORAGE_ABSENT: return "STORAGE_ABSENT";
    case ESP_VISION_STORAGE_UNSUPPORTED: return "STORAGE_UNSUPPORTED";
    case ESP_VISION_STORAGE_UNRECOGNIZED: return "STORAGE_UNRECOGNIZED";
    case ESP_VISION_STORAGE_BLANK: return "STORAGE_UNINITIALIZED";
    case ESP_VISION_STORAGE_IO_ERROR: return "STORAGE_IO_ERROR";
    case ESP_VISION_STORAGE_FORMAT_INTERRUPTED: return "STORAGE_FORMAT_INTERRUPTED";
    case ESP_VISION_STORAGE_STATE_ERROR: return "STORAGE_STATE_UNAVAILABLE";
    case ESP_VISION_STORAGE_FORMATTING: return "STORAGE_BUSY";
    case ESP_VISION_STORAGE_UNMOUNTED: return "STORAGE_NOT_MOUNTED";
    default: return "STORAGE_UNAVAILABLE";
    }
}

static inline const char *esp_vision_storage_status_message(esp_vision_storage_state_t state)
{
    switch (state) {
    case ESP_VISION_STORAGE_READY: return "Filesystem mounted";
    case ESP_VISION_STORAGE_ABSENT: return "Storage absent; check the medium and retry";
    case ESP_VISION_STORAGE_UNSUPPORTED: return "Storage operation unsupported on this board";
    case ESP_VISION_STORAGE_BLANK: return "Blank storage; initialization requires explicit user confirmation";
    case ESP_VISION_STORAGE_UNRECOGNIZED: return "Filesystem unrecognized; preserve or back up data before choosing whether to format";
    case ESP_VISION_STORAGE_IO_ERROR: return "Storage I/O failed; check hardware and preserve data before recovery";
    case ESP_VISION_STORAGE_FORMAT_INTERRUPTED: return "Formatting was interrupted; data access is blocked until the user chooses recovery";
    case ESP_VISION_STORAGE_STATE_ERROR: return "Recovery state unavailable; preserve NVS and storage data for diagnosis";
    case ESP_VISION_STORAGE_FORMATTING: return "Storage recovery in progress";
    case ESP_VISION_STORAGE_UNMOUNTED: return "Automatic mount disabled; use storage.retry() to mount without formatting";
    default: return "Storage unavailable; inspect storage.status() and preserve data before recovery";
    }
}

#endif /* ESP_VISION_STORAGE_STATUS_H */

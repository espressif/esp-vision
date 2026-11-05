/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_STORAGE_SAFETY_H
#define ESP_VISION_STORAGE_SAFETY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*esp_vision_storage_read_fn)(void *context,
                                           size_t offset,
                                           uint8_t *buffer,
                                           size_t length);

/*
 * Return true only when every byte in a non-empty partition is confirmed erased.
 * The caller supplies bounded scratch storage so embedded callers do not need
 * an additional stack allocation. Any read error fails closed.
 */
static inline bool esp_vision_storage_partition_erased(size_t partition_size,
                                                       uint8_t *buffer,
                                                       size_t buffer_capacity,
                                                       esp_vision_storage_read_fn read_partition,
                                                       void *context)
{
    if (partition_size == 0 || buffer == NULL || buffer_capacity == 0 || read_partition == NULL) {
        return false;
    }

    size_t offset = 0;
    while (offset < partition_size) {
        size_t chunk_len = partition_size - offset;
        if (chunk_len > buffer_capacity) {
            chunk_len = buffer_capacity;
        }
        if (!read_partition(context, offset, buffer, chunk_len)) {
            return false;
        }
        for (size_t index = 0; index < chunk_len; ++index) {
            if (buffer[index] != 0xff) {
                return false;
            }
        }
        offset += chunk_len;
    }
    return true;
}

#endif /* ESP_VISION_STORAGE_SAFETY_H */

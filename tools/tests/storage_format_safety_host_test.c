/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "storage_safety.h"

enum {
    MOCK_FR_INT_ERR = 2,
    MOCK_FR_NO_FILESYSTEM = 13,
};

typedef struct {
    const uint8_t *partition;
    size_t partition_size;
    size_t fail_offset;
    bool fail_read;
    size_t read_count;
    size_t offsets[8];
    size_t lengths[8];
} mock_reader_t;

static bool mock_read(void *context, size_t offset, uint8_t *buffer, size_t length)
{
    mock_reader_t *mock = context;
    assert(mock != NULL);
    assert(buffer != NULL);
    assert(mock->read_count < 8);
    mock->offsets[mock->read_count] = offset;
    mock->lengths[mock->read_count] = length;
    mock->read_count++;

    if ((mock->fail_read && offset == mock->fail_offset) ||
            offset > mock->partition_size || length > mock->partition_size - offset) {
        return false;
    }
    memcpy(buffer, mock->partition + offset, length);
    return true;
}

static void test_all_erased_partition_uses_exact_chunk_offsets(void)
{
    const uint8_t partition[17] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    uint8_t scratch[8];
    mock_reader_t mock = {.partition = partition, .partition_size = sizeof(partition)};

    assert(esp_vision_storage_partition_erased(sizeof(partition), scratch, sizeof(scratch),
                                               mock_read, &mock));
    assert(mock.read_count == 3);
    assert(mock.offsets[0] == 0 && mock.lengths[0] == 8);
    assert(mock.offsets[1] == 8 && mock.lengths[1] == 8);
    assert(mock.offsets[2] == 16 && mock.lengths[2] == 1);
}

static void test_partial_non_erased_partition_fails_closed(void)
{
    uint8_t partition[17];
    memset(partition, 0xff, sizeof(partition));
    partition[11] = 0x7f;
    uint8_t scratch[8];
    mock_reader_t mock = {.partition = partition, .partition_size = sizeof(partition)};

    assert(!esp_vision_storage_partition_erased(sizeof(partition), scratch, sizeof(scratch),
                                                mock_read, &mock));
    assert(mock.read_count == 2);
}

static void test_read_failure_fails_closed(void)
{
    const uint8_t partition[17] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    uint8_t scratch[8];
    mock_reader_t mock = {
        .partition = partition,
        .partition_size = sizeof(partition),
        .fail_offset = 8,
        .fail_read = true,
    };

    assert(!esp_vision_storage_partition_erased(sizeof(partition), scratch, sizeof(scratch),
                                                mock_read, &mock));
    assert(mock.read_count == 2);
}

static void test_empty_partition_is_not_a_blank_factory_volume(void)
{
    const uint8_t byte = 0xff;
    uint8_t scratch[1];
    mock_reader_t mock = {.partition = &byte, .partition_size = 1};

    assert(!esp_vision_storage_partition_erased(0, scratch, sizeof(scratch), mock_read, &mock));
    assert(mock.read_count == 0);
    assert(!esp_vision_storage_partition_erased(1, scratch, 0, mock_read, &mock));
    assert(!esp_vision_storage_partition_erased(1, scratch, sizeof(scratch), NULL, &mock));
}

static void test_only_erased_factory_ffat_with_no_filesystem_can_format(void)
{
    assert(esp_vision_storage_should_format_factory_ffat(MOCK_FR_NO_FILESYSTEM,
                                                         MOCK_FR_NO_FILESYSTEM,
                                                         true,
                                                         true));
    assert(!esp_vision_storage_should_format_factory_ffat(MOCK_FR_NO_FILESYSTEM,
                                                          MOCK_FR_NO_FILESYSTEM,
                                                          true,
                                                          false));
    assert(!esp_vision_storage_should_format_factory_ffat(MOCK_FR_NO_FILESYSTEM,
                                                          MOCK_FR_NO_FILESYSTEM,
                                                          false,
                                                          true));
    assert(!esp_vision_storage_should_format_factory_ffat(MOCK_FR_INT_ERR,
                                                          MOCK_FR_NO_FILESYSTEM,
                                                          true,
                                                          true));
}

int main(void)
{
    test_all_erased_partition_uses_exact_chunk_offsets();
    test_partial_non_erased_partition_fails_closed();
    test_read_failure_fails_closed();
    test_empty_partition_is_not_a_blank_factory_volume();
    test_only_erased_factory_ffat_with_no_filesystem_can_format();
    return 0;
}

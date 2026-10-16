# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile the production SD-card sync operation with and without SDMMC."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class StorageSdcardSyncTests(unittest.TestCase):
    def test_board_and_soc_support_and_sync_errors(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")
        source = (ROOT / "platform/storage.c").read_text()
        start = source.index("esp_err_t esp_vision_storage_sdcard_sync(void)")
        end = source.index("\nesp_err_t esp_vision_storage_refresh_volume", start)
        operation = source[start:end]
        harness = r'''
#include <assert.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NOT_SUPPORTED 2
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
static int card;
static int *s_sdcard = &card;
static uint32_t block_count;
static esp_err_t lock_result;
static esp_err_t status_result;
static int lock_calls;
static int unlock_calls;
static int status_calls;
static uint32_t esp_vision_storage_sdcard_block_count(void) { return block_count; }
static esp_err_t esp_vision_storage_sdcard_lock(void) { ++lock_calls; return lock_result; }
static void esp_vision_storage_sdcard_unlock(void) { ++unlock_calls; }
static esp_err_t sdmmc_get_status(int *value) {
    assert(value == s_sdcard);
    ++status_calls;
    return status_result;
}
#endif
''' + operation + r'''
int main(void) {
#if MICROPY_HW_ENABLE_SDCARD && SOC_SDMMC_HOST_SUPPORTED
    assert(esp_vision_storage_sdcard_sync() == ESP_ERR_INVALID_STATE);
    assert(lock_calls == 0 && unlock_calls == 0 && status_calls == 0);
    block_count = 32;
    lock_result = ESP_ERR_INVALID_STATE;
    assert(esp_vision_storage_sdcard_sync() == ESP_ERR_INVALID_STATE);
    assert(lock_calls == 1 && unlock_calls == 0 && status_calls == 0);
    lock_result = ESP_OK;
    assert(esp_vision_storage_sdcard_sync() == ESP_OK);
    assert(lock_calls == 2 && unlock_calls == 1 && status_calls == 1);
    status_result = 17;
    assert(esp_vision_storage_sdcard_sync() == 17);
    assert(lock_calls == 3 && unlock_calls == 2 && status_calls == 2);
#else
    assert(esp_vision_storage_sdcard_sync() == ESP_ERR_NOT_SUPPORTED);
#endif
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="storage-sync-") as temporary:
            path = Path(temporary)
            unit = path / "storage_sync.c"
            unit.write_text(harness)
            for board_enabled, soc_supported in ((0, 0), (0, 1), (1, 0), (1, 1)):
                with self.subTest(board_enabled=board_enabled, soc_supported=soc_supported):
                    executable = path / f"sync-{board_enabled}-{soc_supported}"
                    subprocess.run([
                        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        f"-DMICROPY_HW_ENABLE_SDCARD={board_enabled}",
                        f"-DSOC_SDMMC_HOST_SUPPORTED={soc_supported}",
                        str(unit), "-o", str(executable),
                    ], check=True)
                    subprocess.run([str(executable)], check=True)

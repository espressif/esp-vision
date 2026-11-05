/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <string.h>

#include "storage.h"
#include "ff.h"

static bool allowed = true, finish_ok = true;
static unsigned gets, sets, finishes;
static FRESULT get_result = FR_OK, set_result = FR_OK;
static char existing[45], assigned[14];
static const char *expected_drive = "0:";

bool esp_vision_msc_writer_vfs_begin(esp_vision_storage_volume_t volume, bool writing)
{
    assert(volume == ESP_VISION_STORAGE_VOLUME_FLASH || volume == ESP_VISION_STORAGE_VOLUME_SD);
    assert(writing);
    return allowed;
}

bool esp_vision_msc_writer_vfs_finish(esp_vision_storage_volume_t volume)
{
    (void)volume;
    finishes++;
    return finish_ok;
}

FRESULT f_getlabel(const TCHAR *drive, TCHAR *label, DWORD *serial)
{
    assert(strcmp(drive, expected_drive) == 0 && serial == NULL);
    gets++;
    // Exercise the full Unicode-sized buffer, not just ASCII defaults.
    memcpy(label, existing, sizeof(existing));
    return get_result;
}

FRESULT f_setlabel(const TCHAR *label)
{
    assert(strlen(label) < sizeof(assigned));
    strcpy(assigned, label);
    sets++;
    return set_result;
}

int main(void)
{
    assert(esp_vision_storage_set_default_label(0, "0:", ESP_VISION_STORAGE_FLASH_LABEL) == ESP_OK);
    assert(sets == 1 && strcmp(assigned, "0:ESP_FLASH") == 0 && finishes == 1);
    expected_drive = "1:";
    assert(esp_vision_storage_set_default_label(1, "1:", ESP_VISION_STORAGE_SDCARD_LABEL) == ESP_OK);
    assert(sets == 2 && strcmp(assigned, "1:ESP_SDCARD") == 0);
    memset(existing, 'x', sizeof(existing) - 1);
    assert(esp_vision_storage_set_default_label(1, "1:", ESP_VISION_STORAGE_SDCARD_LABEL) == ESP_OK);
    assert(sets == 2); // Preserve existing labels, including Unicode encodings.
    existing[0] = '\0';
    allowed = false;
    unsigned before = gets;
    assert(esp_vision_storage_set_default_label(1, "1:", ESP_VISION_STORAGE_SDCARD_LABEL) != ESP_OK);
    assert(gets == before && sets == 2 && finishes == 3);
    allowed = true;
    get_result = FR_DISK_ERR;
    assert(esp_vision_storage_set_default_label(1, "1:", ESP_VISION_STORAGE_SDCARD_LABEL) != ESP_OK);
    assert(sets == 2 && finishes == 4);
    get_result = FR_OK;
    set_result = FR_DISK_ERR;
    assert(esp_vision_storage_set_default_label(1, "1:", ESP_VISION_STORAGE_SDCARD_LABEL) != ESP_OK);
    assert(sets == 3 && finishes == 5);
    set_result = FR_OK;
    finish_ok = false;
    assert(esp_vision_storage_set_default_label(1, "1:", ESP_VISION_STORAGE_SDCARD_LABEL) != ESP_OK);
    assert(finishes == 6);
    before = gets;
    const char *invalid[] = {"esp-vision.flash", "esp.flash", "", "TOO_LONG_LABEL"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(esp_vision_storage_set_default_label(1, "1:", invalid[i]) == ESP_ERR_INVALID_ARG);
    }
    assert(esp_vision_storage_set_default_label(1, NULL, "OK") == ESP_ERR_INVALID_ARG);
    assert(esp_vision_storage_set_default_label(1, "1:/", "OK") == ESP_ERR_INVALID_ARG);
    assert(gets == before);
    return 0;
}

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include "storage.h"
#define SOC_SDMMC_HOST_SUPPORTED 1
#define MICROPY_HW_ENABLE_SDCARD 1
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))

typedef intptr_t mp_obj_t;
typedef struct {
    mp_obj_t confirm;
} mp_map_t;
typedef union {
    mp_obj_t u_obj;
} mp_arg_val_t;
typedef struct {
    int qstr;
    int flags;
    mp_arg_val_t defval;
} mp_arg_t;
#define MP_OBJ_NULL 0
#define mp_const_false 2
#define mp_const_true 6
#define MP_QSTR_volume 1
#define MP_QSTR_confirm 2
#define MP_ARG_OBJ 1
#define MP_ARG_KW_ONLY 2
#define MP_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MP_ERROR_TEXT(a) (a)
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)

static void *s_recovery_lock = (void *)1;
static void *s_flash_mutex = (void *)2;
static bool s_flash_mounted;
static bool lock_held, fail_lock, writer_allowed, finish_allowed;
static unsigned handles, format_calls, begin_calls, finish_calls;
static int s_status_mux;
static esp_vision_storage_status_t s_flash_status;
static bool s_sdcard_mounted, sd_present;
static esp_err_t s_sdcard_error;
static int s_sdcard_fatfs_result = -1;
static esp_vision_storage_state_t s_sdcard_mount_state = ESP_VISION_STORAGE_UNAVAILABLE;
static esp_err_t format_result;
static jmp_buf exception;

static _Noreturn void mp_raise_ValueError(const char *message)
{
    assert(strstr(message, "confirm=True") != NULL);
    longjmp(exception, 1);
}

static void mp_arg_parse_all(size_t count, const mp_obj_t *pos, mp_map_t *keywords,
                             size_t allowed_count, const mp_arg_t *allowed, mp_arg_val_t *out)
{
    assert(allowed_count == 2 && count <= 1);
    for (size_t i = 0; i < allowed_count; ++i) {
        out[i] = allowed[i].defval;
    }
    if (count) {
        out[0].u_obj = pos[0];
    }
    if (keywords != NULL) {
        out[1].u_obj = keywords->confirm;
    }
}

static esp_vision_storage_volume_t py_storage_volume(mp_obj_t value)
{
    return value == 99 ? ESP_VISION_STORAGE_VOLUME_SD : ESP_VISION_STORAGE_VOLUME_FLASH;
}

static mp_obj_t py_storage_result(esp_vision_storage_volume_t volume, esp_err_t ret, bool before)
{
    (void)volume;
    (void)before;
    return ret;
}

bool esp_vision_sdcard_is_present(void)
{
    return sd_present;
}

bool esp_vision_storage_sdcard_is_ready(void)
{
    assert(!"status query issued a hardware readiness command");
    return false;
}

static int xSemaphoreTake(void *lock, unsigned timeout)
{
    assert(lock == s_recovery_lock && timeout == 0);
    if (fail_lock || lock_held) {
        return 0;
    }
    lock_held = true;
    return pdTRUE;
}

static void xSemaphoreGive(void *lock)
{
    assert(lock == s_recovery_lock && lock_held);
    lock_held = false;
}

uint32_t esp_vision_storage_vfs_open_handle_count(esp_vision_storage_volume_t volume)
{
    assert(volume == ESP_VISION_STORAGE_VOLUME_FLASH);
    return handles;
}

bool esp_vision_msc_writer_vfs_begin(esp_vision_storage_volume_t volume, bool writing)
{
    assert(volume == ESP_VISION_STORAGE_VOLUME_FLASH && writing && lock_held);
    ++begin_calls;
    return writer_allowed;
}

bool esp_vision_msc_writer_vfs_finish(esp_vision_storage_volume_t volume)
{
    assert(volume == ESP_VISION_STORAGE_VOLUME_FLASH && lock_held);
    ++finish_calls;
    return finish_allowed;
}

static esp_err_t esp_vision_storage_register_flash_vfs(bool confirmed, bool automatic)
{
    assert(!automatic);
    assert(confirmed && lock_held && !s_flash_mounted && writer_allowed && handles == 0);
    ++format_calls;
    return format_result;
}

/* PRODUCTION_FUNCTIONS */

static void reset(void)
{
    s_flash_mounted = lock_held = fail_lock = false;
    writer_allowed = finish_allowed = true;
    handles = format_calls = begin_calls = finish_calls = 0;
    format_result = ESP_OK;
}

int main(void)
{
    const mp_obj_t not_confirmed[] = {MP_OBJ_NULL, mp_const_false, 1, 17, 999};
    for (size_t i = 0; i < MP_ARRAY_SIZE(not_confirmed); ++i) {
        reset();
        mp_map_t keywords = {.confirm = not_confirmed[i]};
        if (setjmp(exception) == 0) {
            (void)py_storage_format(0, NULL, &keywords);
            assert(!"confirmation was not rejected");
        }
        assert(format_calls == 0 && begin_calls == 0);
    }
    reset();
    assert(esp_vision_storage_format_flash(false) == ESP_ERR_INVALID_ARG && format_calls == 0);
    s_flash_mounted = true;
    assert(esp_vision_storage_format_flash(true) == ESP_ERR_INVALID_STATE && !lock_held && format_calls == 0);
    s_flash_mounted = false;
    handles = 1;
    assert(esp_vision_storage_format_flash(true) == ESP_ERR_INVALID_STATE && !lock_held && format_calls == 0);
    handles = 0;
    writer_allowed = false;
    assert(esp_vision_storage_format_flash(true) == ESP_ERR_INVALID_STATE && !lock_held && format_calls == 0);
    writer_allowed = true;
    fail_lock = true;
    assert(esp_vision_storage_format_flash(true) == ESP_ERR_INVALID_STATE && format_calls == 0);
    reset();
    mp_map_t approved = {.confirm = mp_const_true};
    assert(py_storage_format(0, NULL, &approved) == ESP_OK);
    assert(format_calls == 1 && finish_calls == 1 && !lock_held);
    format_result = ESP_FAIL;
    assert(py_storage_format(0, NULL, &approved) == ESP_FAIL);
    assert(format_calls == 2 && finish_calls == 2 && !lock_held);
    finish_allowed = false;
    format_result = ESP_OK;
    assert(py_storage_format(0, NULL, &approved) == ESP_ERR_INVALID_STATE && !lock_held);
    mp_obj_t sd = 99;
    unsigned before = format_calls;
    assert(py_storage_format(1, &sd, &approved) == ESP_ERR_NOT_SUPPORTED && format_calls == before);
    esp_vision_storage_status_t status;
    s_sdcard_mounted = sd_present = true;
    s_sdcard_error = ESP_FAIL;
    esp_vision_storage_get_status(ESP_VISION_STORAGE_VOLUME_SD, &status);
    assert(status.supported && status.present && status.state == ESP_VISION_STORAGE_IO_ERROR);
    s_sdcard_error = ESP_OK;
    esp_vision_storage_get_status(ESP_VISION_STORAGE_VOLUME_SD, &status);
    assert(status.state == ESP_VISION_STORAGE_READY);
    sd_present = s_sdcard_mounted = false;
    esp_vision_storage_get_status(ESP_VISION_STORAGE_VOLUME_SD, &status);
    assert(status.state == ESP_VISION_STORAGE_ABSENT);
    esp_vision_storage_get_status((esp_vision_storage_volume_t)99, &status);
    assert(!status.supported && status.state == ESP_VISION_STORAGE_UNSUPPORTED);
    return 0;
}

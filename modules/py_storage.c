/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "py/runtime.h"
#include "storage.h"

static esp_vision_storage_volume_t py_storage_volume(mp_obj_t value)
{
    if (value == MP_OBJ_NULL) {
        return ESP_VISION_STORAGE_VOLUME_FLASH;
    }
    const char *name = mp_obj_str_get_str(value);
    if (strcmp(name, "flash") == 0) {
        return ESP_VISION_STORAGE_VOLUME_FLASH;
    }
    if (strcmp(name, "sd") == 0) {
        return ESP_VISION_STORAGE_VOLUME_SD;
    }
    mp_raise_ValueError(MP_ERROR_TEXT("volume must be flash or sd"));
}

static mp_obj_t py_storage_status_dict(esp_vision_storage_volume_t volume)
{
    esp_vision_storage_status_t status;
    esp_vision_storage_get_status(volume, &status);
    mp_obj_t result = mp_obj_new_dict(13);
    const char *name = volume == ESP_VISION_STORAGE_VOLUME_FLASH ? "flash" : "sd";
    const char *state = esp_vision_storage_state_name(status.state);
    const char *code = esp_vision_storage_status_code(status.state);
    const char *message = esp_vision_storage_status_message(status.state);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_volume), mp_obj_new_str(name, strlen(name)));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_state), mp_obj_new_str(state, strlen(state)));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_code), mp_obj_new_str(code, strlen(code)));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_message), mp_obj_new_str(message, strlen(message)));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_supported), mp_obj_new_bool(status.supported));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_present), mp_obj_new_bool(status.present));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_mounted), mp_obj_new_bool(status.mounted));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_format_supported), mp_obj_new_bool(status.format_supported));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_offset), mp_obj_new_int_from_uint(status.address));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_size), mp_obj_new_int_from_uint(status.size));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_error), mp_obj_new_int(status.error));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_fatfs_result), mp_obj_new_int(status.fatfs_result));
    return result;
}

static mp_obj_t py_storage_status(size_t n_args, const mp_obj_t *args)
{
    return py_storage_status_dict(py_storage_volume(n_args ? args[0] : MP_OBJ_NULL));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_storage_status_obj, 0, 1, py_storage_status);

static mp_obj_t py_storage_result(esp_vision_storage_volume_t volume, esp_err_t error, bool was_mounted)
{
    mp_obj_t result = py_storage_status_dict(volume);
    esp_vision_storage_status_t status;
    esp_vision_storage_get_status(volume, &status);
    const char *operation_error = esp_err_to_name(error);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_ok), mp_obj_new_bool(error == ESP_OK));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_operation_error), mp_obj_new_str(operation_error, strlen(operation_error)));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_soft_reset_required),
                      mp_obj_new_bool(error == ESP_OK && status.mounted && !was_mounted));
    return result;
}

static mp_obj_t py_storage_retry(size_t n_args, const mp_obj_t *args)
{
    esp_vision_storage_volume_t volume = py_storage_volume(n_args ? args[0] : MP_OBJ_NULL);
    esp_vision_storage_status_t before;
    esp_vision_storage_get_status(volume, &before);
    esp_err_t ret = esp_vision_storage_retry(volume);
    return py_storage_result(volume, ret, before.mounted);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_storage_retry_obj, 0, 1, py_storage_retry);

static mp_obj_t py_storage_format(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_volume, ARG_confirm };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_volume, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_confirm, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = mp_const_false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    esp_vision_storage_volume_t volume = py_storage_volume(args[ARG_volume].u_obj);
    // Require the actual boolean True, not a truthy string or omitted flag.
    if (args[ARG_confirm].u_obj != mp_const_true) {
        mp_raise_ValueError(MP_ERROR_TEXT("format erases files; back up required data and pass confirm=True only after user approval"));
    }
    esp_vision_storage_status_t before;
    esp_vision_storage_get_status(volume, &before);
    esp_err_t ret = volume == ESP_VISION_STORAGE_VOLUME_FLASH
                    ? esp_vision_storage_format_flash(true) : ESP_ERR_NOT_SUPPORTED;
    return py_storage_result(volume, ret, before.mounted);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(py_storage_format_obj, 0, py_storage_format);

static const mp_rom_map_elem_t py_storage_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_storage) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&py_storage_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_retry), MP_ROM_PTR(&py_storage_retry_obj) },
    { MP_ROM_QSTR(MP_QSTR_format), MP_ROM_PTR(&py_storage_format_obj) },
};
static MP_DEFINE_CONST_DICT(py_storage_globals, py_storage_globals_table);

const mp_obj_module_t py_storage_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *) &py_storage_globals,
};
MP_REGISTER_MODULE(MP_QSTR_storage, py_storage_module);

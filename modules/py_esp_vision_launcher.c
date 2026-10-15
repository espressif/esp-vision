/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <string.h>

#include "py/nlr.h"
#include "py/obj.h"
#include "py/objmodule.h"
#include "py/objstr.h"
#include "py/qstr.h"
#include "py/runtime.h"

#include "ev_launcher_state.h"

static const char *launcher_string(mp_obj_t value, size_t max_len, size_t *out_len)
{
    size_t length = 0;
    const char *text = mp_obj_str_get_data(value, &length);
    if (length == 0 || length > max_len || memchr(text, '\0', length) != NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid launcher state argument"));
    }
    if (out_len != NULL) {
        *out_len = length;
    }
    return text;
}

static uint64_t launcher_generation(mp_obj_t value)
{
    size_t length = 0;
    const char *text = launcher_string(value, 16, &length);
    uint64_t result = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9' ||
                result > (UINT64_MAX - (uint64_t)(text[i] - '0')) / 10) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid launcher generation"));
        }
        result = result * 10 + (uint64_t)(text[i] - '0');
    }
    if (result == 0 || result > UINT64_C(9007199254740991)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid launcher generation"));
    }
    return result;
}

static bool launcher_is_keyboard_interrupt(void *raw_exception)
{
    if (raw_exception == NULL) {
        return false;
    }
    mp_obj_t exception = MP_OBJ_FROM_PTR(raw_exception);
    if (!mp_obj_is_exception_instance(exception)) {
        return false;
    }
    mp_obj_t type = MP_OBJ_FROM_PTR(((mp_obj_base_t *)raw_exception)->type);
    return mp_obj_is_subclass_fast(type, MP_OBJ_FROM_PTR(&mp_type_KeyboardInterrupt));
}

static bool launcher_identifier_part(const char *text, size_t length)
{
    if (length == 0 || !((text[0] >= 'A' && text[0] <= 'Z') ||
                         (text[0] >= 'a' && text[0] <= 'z') || text[0] == '_')) {
        return false;
    }
    for (size_t i = 1; i < length; ++i) {
        const char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

static bool launcher_candidate_name_valid(const char *name, size_t length)
{
    if (length > 256) {
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i == length || name[i] == '.') {
            if (!launcher_identifier_part(name + start, i - start)) {
                return false;
            }
            start = i + 1;
        } else if ((unsigned char)name[i] >= 0x80) {
            return false;
        }
    }
    return true;
}

static bool launcher_candidate_path_valid(const char *path, size_t length)
{
    if (length > EV_LAUNCHER_SOURCE_PATH_MAX ||
            !(strncmp(path, "/lib/", 5) == 0 || strncmp(path, "/sdcard/lib/", 12) == 0) ||
            length < 4 || memcmp(path + length - 3, ".py", 3) != 0 ||
            strstr(path, "//") != NULL || memchr(path, '\\', length) != NULL) {
        return false;
    }
    size_t start = 1;
    for (size_t i = 1; i <= length; ++i) {
        if (i == length || path[i] == '/') {
            const size_t segment_length = i - start;
            if ((segment_length == 1 && path[start] == '.') ||
                    (segment_length == 2 && path[start] == '.' && path[start + 1] == '.')) {
                return false;
            }
            start = i + 1;
        }
    }
    return true;
}

static bool launcher_candidate_binding_matches(const char *name, size_t name_length,
                                               const char *path, size_t path_length, bool is_package)
{
    const size_t root_length = strncmp(path, "/sdcard/lib/", 12) == 0 ? 12 : 5;
    const size_t suffix_length = is_package ? sizeof("/__init__.py") - 1 : sizeof(".py") - 1;
    if (path_length - root_length != name_length + suffix_length) {
        return false;
    }
    for (size_t i = 0; i < name_length; ++i) {
        const char expected = name[i] == '.' ? '/' : name[i];
        if (path[root_length + i] != expected) {
            return false;
        }
    }
    const char *suffix = is_package ? "/__init__.py" : ".py";
    return memcmp(path + root_length + name_length, suffix, suffix_length) == 0;
}

// Create a genuine MicroPython module and return its shared namespace.  The
// generated fixed importer executes source into this dictionary so function
// globals and module attributes remain the same object, as with normal imports.
static mp_obj_t launcher_new_candidate_module(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    size_t name_length = 0, path_length = 0;
    const char *name = launcher_string(args[0], 256, &name_length);
    const char *path = launcher_string(args[1], EV_LAUNCHER_SOURCE_PATH_MAX, &path_length);
    const bool is_package = args[2] == mp_const_true;
    if (!launcher_candidate_name_valid(name, name_length) ||
            !launcher_candidate_path_valid(path, path_length) ||
            (args[2] != mp_const_true && args[2] != mp_const_false) ||
            !launcher_candidate_binding_matches(name, name_length, path, path_length, is_package)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid candidate module metadata"));
    }

    const qstr module_name = qstr_from_strn(name, name_length);
    const mp_obj_t module = mp_obj_new_module(module_name);
    mp_obj_t globals = MP_OBJ_FROM_PTR(mp_obj_module_get_globals(module));
    const char *last_dot = NULL;
    for (size_t i = 0; i < name_length; ++i) if (name[i] == '.') {
            last_dot = name + i;
        }
    const char *package_name = is_package ? name : (last_dot == NULL ? "" : name);
    const size_t package_length = is_package ? name_length : (last_dot == NULL ? 0 : (size_t)(last_dot - name));
    mp_obj_dict_store(globals, MP_OBJ_NEW_QSTR(qstr_from_strn("__file__", sizeof("__file__") - 1)), args[1]);
    mp_obj_dict_store(globals, MP_OBJ_NEW_QSTR(qstr_from_strn("__package__", sizeof("__package__") - 1)), mp_obj_new_str(package_name, package_length));
    if (is_package) {
        const char *slash = path + path_length;
        while (slash > path && slash[-1] != '/') {
            --slash;
        }
        if (slash == path || slash[-1] != '/') {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid candidate package path"));
        }
        mp_obj_t package_path = mp_obj_new_str(path, (size_t)(slash - path - 1));
        mp_obj_dict_store(globals, MP_OBJ_NEW_QSTR(qstr_from_strn("__path__", sizeof("__path__") - 1)), package_path);
    }
    mp_obj_t result[2] = { module, globals };
    return mp_obj_new_tuple(2, result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(launcher_new_candidate_module_obj, 3, 3, launcher_new_candidate_module);

static mp_obj_t launcher_execute_active(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    const uint64_t generation = launcher_generation(args[0]);
    const char *hash = launcher_string(args[1], EV_LAUNCHER_SOURCE_HASH_HEX_LEN, NULL);
    const char *path = launcher_string(args[2], EV_LAUNCHER_SOURCE_PATH_MAX, NULL);
    const mp_obj_t callable = args[3];
    if (!mp_obj_is_callable(callable) ||
            !esp_vision_launcher_begin_active(generation, hash, path)) {
        mp_raise_ValueError(MP_ERROR_TEXT("active App runner is not authorized"));
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t result = mp_call_function_0(callable);
        nlr_pop();
        if (!esp_vision_launcher_finish_active(EV_LAUNCHER_OUTCOME_NORMAL_RETURN)) {
            mp_raise_ValueError(MP_ERROR_TEXT("active App outcome is unavailable"));
        }
        return result;
    }

    const ev_launcher_outcome_t outcome = launcher_is_keyboard_interrupt(nlr.ret_val)
                                          ? EV_LAUNCHER_OUTCOME_USER_INTERRUPTION
                                          : EV_LAUNCHER_OUTCOME_UNCAUGHT_EXCEPTION;
    (void)esp_vision_launcher_finish_active(outcome);
    nlr_jump(nlr.ret_val);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(launcher_execute_active_obj, 4, 4, launcher_execute_active);

static mp_obj_t launcher_execute_trial(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    const char *operation = launcher_string(args[0], EV_LAUNCHER_OPERATION_ID_LEN, NULL);
    const char *path = launcher_string(args[1], EV_LAUNCHER_SOURCE_PATH_MAX, NULL);
    const char *hash = launcher_string(args[2], EV_LAUNCHER_SOURCE_HASH_HEX_LEN, NULL);
    const mp_obj_t callable = args[3];
    if (!mp_obj_is_callable(callable) ||
            !esp_vision_launcher_begin_trial(operation, path, hash)) {
        mp_raise_ValueError(MP_ERROR_TEXT("trial runner is not authorized"));
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t result = mp_call_function_0(callable);
        nlr_pop();
        if (!esp_vision_launcher_finish_trial(EV_LAUNCHER_OUTCOME_NORMAL_RETURN)) {
            mp_raise_ValueError(MP_ERROR_TEXT("trial outcome is unavailable"));
        }
        return result;
    }

    const ev_launcher_outcome_t outcome = launcher_is_keyboard_interrupt(nlr.ret_val)
                                          ? EV_LAUNCHER_OUTCOME_USER_INTERRUPTION
                                          : EV_LAUNCHER_OUTCOME_UNCAUGHT_EXCEPTION;
    (void)esp_vision_launcher_finish_trial(outcome);
    nlr_jump(nlr.ret_val);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(launcher_execute_trial_obj, 4, 4, launcher_execute_trial);

static const mp_rom_map_elem_t launcher_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_esp_vision_launcher) },
    { MP_ROM_QSTR(MP_QSTR_execute_active), MP_ROM_PTR(&launcher_execute_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_execute_trial), MP_ROM_PTR(&launcher_execute_trial_obj) },
    { MP_ROM_QSTR(MP_QSTR__new_candidate_module), MP_ROM_PTR(&launcher_new_candidate_module_obj) },
};
static MP_DEFINE_CONST_DICT(launcher_module_globals, launcher_module_globals_table);

const mp_obj_module_t mp_module_esp_vision_launcher = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *) &launcher_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_esp_vision_launcher, mp_module_esp_vision_launcher);

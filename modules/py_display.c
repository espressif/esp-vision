/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_err.h"

#include "py/obj.h"
#include "py/runtime.h"

#include "display.h"
#include "fb_alloc.h"
#include "py_helper.h"

typedef struct {
    mp_obj_base_t base;
    bool active;
} esp_vision_display_obj_t;

// The board display is a singleton, but each Display() call creates a Python
// wrapper.  Keep ownership with the newest live wrapper so a delayed finaliser
// from an earlier raw-REPL script cannot tear down the display reused by the
// next script.
static esp_vision_display_obj_t *s_display_owner;

static void py_display_raise_esp_err(esp_err_t err)
{
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("display error: %s"), esp_err_to_name(err));
}

static mp_obj_t py_display_make_new(const mp_obj_type_t *type,
                                    size_t n_args,
                                    size_t n_kw,
                                    const mp_obj_t *all_args)
{
    enum {
        ARG_width,
        ARG_height,
        ARG_refresh,
        ARG_backlight,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_width, MP_ARG_INT, {.u_int = 0}},
        {MP_QSTR_height, MP_ARG_INT, {.u_int = 0}},
        {MP_QSTR_refresh, MP_ARG_INT, {.u_int = 60}},
        {MP_QSTR_backlight, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 100}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    (void)args[ARG_refresh];

    // Allocate the wrapper before touching the singleton.  If this allocation
    // triggers GC, the previous owner's finaliser runs first and the hardware
    // is then initialized from a clean state below.
    esp_vision_display_obj_t *self = mp_obj_malloc_with_finaliser(esp_vision_display_obj_t, type);
    memset(self, 0, sizeof(*self));
    self->base.type = type;

    esp_err_t ret = esp_vision_display_init((uint32_t)args[ARG_width].u_int,
                                            (uint32_t)args[ARG_height].u_int,
                                            (uint32_t)args[ARG_backlight].u_int);
    if (ret != ESP_OK) {
        py_display_raise_esp_err(ret);
    }

    self->active = true;
    s_display_owner = self;
    return MP_OBJ_FROM_PTR(self);
}

static void py_display_ensure_ready(void)
{
    if (!esp_vision_display_is_ready()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("display is not initialized"));
    }
}

static mp_obj_t py_display_deinit(mp_obj_t self_in)
{
    esp_vision_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->active) {
        self->active = false;
        if (s_display_owner == self) {
            s_display_owner = NULL;
            esp_vision_display_deinit();
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_display_deinit_obj, py_display_deinit);

static mp_obj_t py_display_width(mp_obj_t self_in)
{
    (void)self_in;
    py_display_ensure_ready();
    return mp_obj_new_int_from_uint(esp_vision_display_get_width());
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_display_width_obj, py_display_width);

static mp_obj_t py_display_height(mp_obj_t self_in)
{
    (void)self_in;
    py_display_ensure_ready();
    return mp_obj_new_int_from_uint(esp_vision_display_get_height());
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_display_height_obj, py_display_height);

static mp_obj_t py_display_backlight(size_t n_args, const mp_obj_t *args)
{
    py_display_ensure_ready();

    if (n_args == 1) {
        return mp_obj_new_int_from_uint(esp_vision_display_get_backlight());
    }

    esp_err_t ret = esp_vision_display_set_backlight((uint32_t)mp_obj_get_int(args[1]));
    if (ret != ESP_OK) {
        py_display_raise_esp_err(ret);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_display_backlight_obj, 1, 2, py_display_backlight);

static mp_obj_t py_display_clear(size_t n_args, const mp_obj_t *args)
{
    py_display_ensure_ready();
    bool display_off = (n_args > 1) && mp_obj_is_true(args[1]);
    esp_err_t ret = esp_vision_display_clear(display_off);
    if (ret != ESP_OK) {
        py_display_raise_esp_err(ret);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_display_clear_obj, 1, 2, py_display_clear);

static mp_obj_t py_display_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_image,
        ARG_x,
        ARG_y,
        ARG_x_scale,
        ARG_y_scale,
        ARG_roi,
        ARG_fit,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_image, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_rom_obj = MP_ROM_NONE}},
        {MP_QSTR_x, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0}},
        {MP_QSTR_y, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0}},
        {MP_QSTR_x_scale, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE}},
        {MP_QSTR_y_scale, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE}},
        {MP_QSTR_roi, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE}},
        {MP_QSTR_fit, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true}},
    };

    (void)pos_args[0];
    py_display_ensure_ready();

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    fb_alloc_mark();
    image_t *img = py_helper_arg_to_image(args[ARG_image].u_obj, ARG_IMAGE_ANY | ARG_IMAGE_ALLOC);
    rectangle_t roi = py_helper_arg_to_roi(args[ARG_roi].u_obj, img);

    esp_vision_display_draw_config_t config = {
        .x = args[ARG_x].u_int,
        .y = args[ARG_y].u_int,
        .x_scale = 1.0f,
        .y_scale = 1.0f,
        .fit = args[ARG_fit].u_bool,
        .roi = &roi,
    };
    py_helper_arg_to_scale(args[ARG_x_scale].u_obj, args[ARG_y_scale].u_obj, &config.x_scale, &config.y_scale);

    esp_err_t ret = esp_vision_display_write(img, &config);
    fb_alloc_free_till_mark();
    if (ret != ESP_OK) {
        py_display_raise_esp_err(ret);
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(py_display_write_obj, 1, py_display_write);

static const mp_rom_map_elem_t py_display_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&py_display_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&py_display_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_width), MP_ROM_PTR(&py_display_width_obj)},
    {MP_ROM_QSTR(MP_QSTR_height), MP_ROM_PTR(&py_display_height_obj)},
    {MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&py_display_clear_obj)},
    {MP_ROM_QSTR(MP_QSTR_backlight), MP_ROM_PTR(&py_display_backlight_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&py_display_write_obj)},
};
static MP_DEFINE_CONST_DICT(py_display_locals_dict, py_display_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    esp_vision_display_type,
    MP_QSTR_ESP32Display,
    MP_TYPE_FLAG_NONE,
    make_new, py_display_make_new,
    locals_dict, &py_display_locals_dict
);

static const mp_rom_map_elem_t display_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_display)},
    {MP_ROM_QSTR(MP_QSTR_Display), MP_ROM_PTR(&esp_vision_display_type)},
    {MP_ROM_QSTR(MP_QSTR_ESP32Display), MP_ROM_PTR(&esp_vision_display_type)},
};
static MP_DEFINE_CONST_DICT(display_module_globals, display_module_globals_table);

const mp_obj_module_t mp_module_display = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *) &display_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_display, mp_module_display);

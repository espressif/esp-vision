/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "py/runtime.h"

#include "lcd_touch.h"

typedef struct {
    mp_obj_base_t base;
    bool active;
} esp_vision_lcd_touch_obj_t;

// As with Display, only the newest wrapper owns the board singleton. An old
// script's delayed finalizer must not release a newer script's LCD touch device.
static esp_vision_lcd_touch_obj_t *s_lcd_touch_owner;

static void py_lcd_touch_raise_esp_err(esp_err_t err)
{
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("lcd_touch error: %s"), esp_err_to_name(err));
}

static mp_obj_t py_lcd_touch_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    (void)args;
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    // Allocate first: GC may finalize the previous owner during allocation.
    esp_vision_lcd_touch_obj_t *self = mp_obj_malloc_with_finaliser(esp_vision_lcd_touch_obj_t, type);
    self->active = false;
    esp_err_t ret = esp_vision_lcd_touch_init();
    if (ret != ESP_OK) {
        py_lcd_touch_raise_esp_err(ret);
    }
    self->active = true;
    s_lcd_touch_owner = self;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t py_lcd_touch_deinit(mp_obj_t self_in)
{
    esp_vision_lcd_touch_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->active) {
        self->active = false;
        if (s_lcd_touch_owner == self) {
            s_lcd_touch_owner = NULL;
            esp_vision_lcd_touch_deinit();
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_lcd_touch_deinit_obj, py_lcd_touch_deinit);

static mp_obj_t py_lcd_touch_read(mp_obj_t self_in)
{
    esp_vision_lcd_touch_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->active || (s_lcd_touch_owner != self)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("lcd_touch object is no longer active"));
    }
    esp_vision_lcd_touch_point_t points[ESP_VISION_LCD_TOUCH_MAX_POINTS];
    uint8_t count = 0;
    esp_err_t ret = esp_vision_lcd_touch_read(points, &count);
    if (ret != ESP_OK) {
        py_lcd_touch_raise_esp_err(ret);
    }
    mp_obj_t result[ESP_VISION_LCD_TOUCH_MAX_POINTS];
    for (uint8_t i = 0; i < count; ++i) {
        mp_obj_t xy[] = {mp_obj_new_int(points[i].x), mp_obj_new_int(points[i].y)};
        result[i] = mp_obj_new_tuple(2, xy);
    }
    return mp_obj_new_tuple(count, result);
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_lcd_touch_read_obj, py_lcd_touch_read);

static const mp_rom_map_elem_t py_lcd_touch_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&py_lcd_touch_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&py_lcd_touch_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&py_lcd_touch_read_obj)},
};
static MP_DEFINE_CONST_DICT(py_lcd_touch_locals_dict, py_lcd_touch_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    esp_vision_lcd_touch_type,
    MP_QSTR_Touch,
    MP_TYPE_FLAG_NONE,
    make_new, py_lcd_touch_make_new,
    locals_dict, &py_lcd_touch_locals_dict
);

static const mp_rom_map_elem_t lcd_touch_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_lcd_touch)},
    {MP_ROM_QSTR(MP_QSTR_Touch), MP_ROM_PTR(&esp_vision_lcd_touch_type)},
};
static MP_DEFINE_CONST_DICT(lcd_touch_module_globals, lcd_touch_module_globals_table);

const mp_obj_module_t mp_module_lcd_touch = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *) &lcd_touch_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lcd_touch, mp_module_lcd_touch);

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "py/mpconfig.h"
#if MICROPY_HW_ENABLE_USBDEV && (!defined(MICROPY_HW_USB_UVC) || MICROPY_HW_USB_UVC)
#include <string.h>
#include "py/runtime.h"
#include "py_image.h"
#include "uvc.h"

static mp_obj_t py_uvc_send(mp_obj_t img, bool fixed)
{
    bool accepted;
    esp_err_t err = esp_vision_uvc_write(py_image_cobj(img), fixed, &accepted);
    if (err == ESP_ERR_INVALID_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("UVC requires 320x240, JPEG <= 48 KiB"));
    }
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("UVC: %s"), esp_err_to_name(err));
    }
    return mp_obj_new_bool(accepted);
}
static mp_obj_t py_uvc_write(mp_obj_t img)
{
    return py_uvc_send(img, false);
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_uvc_write_obj, py_uvc_write);
static mp_obj_t py_uvc_set_frame(mp_obj_t img)
{
    return py_uvc_send(img, true);
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_uvc_set_frame_obj, py_uvc_set_frame);
static mp_obj_t py_uvc_reset(void)
{
    esp_vision_uvc_reset();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_uvc_reset_obj, py_uvc_reset);
static mp_obj_t py_uvc_streaming(void)
{
    return mp_obj_new_bool(esp_vision_uvc_streaming());
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_uvc_streaming_obj, py_uvc_streaming);
static mp_obj_t py_uvc_bind(mp_obj_t source)
{
    bool enabled = source != mp_const_none;
    if (enabled && strcmp(mp_obj_str_get_str(source), "preview") != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("UVC source must be 'preview' or None"));
    }
    esp_vision_uvc_bind_preview(enabled);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_uvc_bind_obj, py_uvc_bind);
static mp_obj_t py_uvc_get_mode(void)
{
    mp_obj_t mode = mp_obj_new_dict(0);
    mp_obj_dict_store(mode, MP_OBJ_NEW_QSTR(MP_QSTR_format), MP_OBJ_NEW_QSTR(MP_QSTR_MJPEG));
    mp_obj_dict_store(mode, MP_OBJ_NEW_QSTR(MP_QSTR_width), MP_OBJ_NEW_SMALL_INT(ESP_VISION_UVC_WIDTH));
    mp_obj_dict_store(mode, MP_OBJ_NEW_QSTR(MP_QSTR_height), MP_OBJ_NEW_SMALL_INT(ESP_VISION_UVC_HEIGHT));
    mp_obj_dict_store(mode, MP_OBJ_NEW_QSTR(MP_QSTR_interval_us), MP_OBJ_NEW_SMALL_INT(ESP_VISION_UVC_INTERVAL_US));
    return mode;
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_uvc_get_mode_obj, py_uvc_get_mode);
static const mp_rom_map_elem_t py_uvc_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_uvc) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&py_uvc_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_frame), MP_ROM_PTR(&py_uvc_set_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&py_uvc_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind), MP_ROM_PTR(&py_uvc_bind_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_streaming), MP_ROM_PTR(&py_uvc_streaming_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_mode), MP_ROM_PTR(&py_uvc_get_mode_obj) },
};
static MP_DEFINE_CONST_DICT(py_uvc_globals, py_uvc_globals_table);
const mp_obj_module_t py_uvc_module = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *) &py_uvc_globals };
MP_REGISTER_MODULE(MP_QSTR_uvc, py_uvc_module);
#endif

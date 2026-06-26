/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"

#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif

#ifndef NO_QSTR
#include "imlib.h"
#endif

#include "camera.h"
#include "py_image.h"

#ifdef NO_QSTR
#define SENSOR_PIXFORMAT_GRAYSCALE (0)
#define SENSOR_PIXFORMAT_RGB565    (0)
#else
#define SENSOR_PIXFORMAT_GRAYSCALE PIXFORMAT_GRAYSCALE
#define SENSOR_PIXFORMAT_RGB565    PIXFORMAT_RGB565
#endif

static uint8_t *s_snapshot_buf = NULL;
static size_t s_snapshot_buf_size = 0;

static void sensor_raise_esp_err(esp_err_t err)
{
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("camera error: %s"), esp_err_to_name(err));
}

static void sensor_release_snapshot_buf(void)
{
    if (s_snapshot_buf != NULL) {
        heap_caps_free(s_snapshot_buf);
        s_snapshot_buf = NULL;
    }
    s_snapshot_buf_size = 0;
}

static void sensor_ensure_camera_ready(void)
{
    if (!esp_vision_camera_is_ready()) {
        esp_err_t ret = esp_vision_camera_init();
        if (ret != ESP_OK) {
            sensor_raise_esp_err(ret);
        }
    }
}

static uint8_t *sensor_get_snapshot_buf(size_t frame_size)
{
    if ((s_snapshot_buf != NULL) && (s_snapshot_buf_size >= frame_size)) {
        return s_snapshot_buf;
    }

    sensor_release_snapshot_buf();
    s_snapshot_buf = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_snapshot_buf == NULL) {
        s_snapshot_buf = heap_caps_malloc(frame_size, MALLOC_CAP_8BIT);
    }
    if (s_snapshot_buf == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate snapshot buffer"));
    }

    s_snapshot_buf_size = frame_size;
    return s_snapshot_buf;
}

static void sensor_drop_frame(uint8_t *pixels, size_t frame_size)
{
    esp_err_t ret = esp_vision_camera_capture(pixels, frame_size);
    if (ret != ESP_OK) {
        sensor_raise_esp_err(ret);
    }
}

static mp_obj_t sensor_reset(void)
{
    sensor_release_snapshot_buf();
    esp_vision_camera_init0();
    esp_err_t ret = esp_vision_camera_init();
    if (ret != ESP_OK) {
        sensor_raise_esp_err(ret);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(sensor_reset_obj, sensor_reset);

static mp_obj_t sensor_shutdown(size_t n_args, const mp_obj_t *args)
{
    bool enable = true;
    if (n_args > 0) {
        enable = mp_obj_is_true(args[0]);
    }

    if (enable) {
        sensor_release_snapshot_buf();
        esp_vision_camera_deinit();
    } else {
        sensor_ensure_camera_ready();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sensor_shutdown_obj, 0, 1, sensor_shutdown);

static mp_obj_t sensor_set_pixformat(mp_obj_t pixformat_in)
{
    esp_err_t ret = esp_vision_camera_set_pixformat((uint32_t)mp_obj_get_int(pixformat_in));
    if (ret != ESP_OK) {
        sensor_raise_esp_err(ret);
    }
    sensor_release_snapshot_buf();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sensor_set_pixformat_obj, sensor_set_pixformat);

static mp_obj_t sensor_get_pixformat(void)
{
    return mp_obj_new_int_from_uint(esp_vision_camera_get_pixformat());
}
static MP_DEFINE_CONST_FUN_OBJ_0(sensor_get_pixformat_obj, sensor_get_pixformat);

static mp_obj_t sensor_set_framesize(mp_obj_t framesize_in)
{
    esp_err_t ret = esp_vision_camera_set_framesize((esp_vision_camera_framesize_t)mp_obj_get_int(framesize_in));
    if (ret != ESP_OK) {
        sensor_raise_esp_err(ret);
    }
    sensor_release_snapshot_buf();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sensor_set_framesize_obj, sensor_set_framesize);

static mp_obj_t sensor_get_framesize(void)
{
    uint32_t width = esp_vision_camera_get_width();
    uint32_t height = esp_vision_camera_get_height();

    if ((width == 160) && (height == 120)) {
        return MP_OBJ_NEW_SMALL_INT(ESP_VISION_CAMERA_FRAMESIZE_QQVGA);
    }
    if ((width == 320) && (height == 240)) {
        return MP_OBJ_NEW_SMALL_INT(ESP_VISION_CAMERA_FRAMESIZE_QVGA);
    }
    if ((width == 640) && (height == 480)) {
        return MP_OBJ_NEW_SMALL_INT(ESP_VISION_CAMERA_FRAMESIZE_VGA);
    }

    mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("unknown frame size"));
}
static MP_DEFINE_CONST_FUN_OBJ_0(sensor_get_framesize_obj, sensor_get_framesize);

static mp_obj_t sensor_width(void)
{
    return mp_obj_new_int_from_uint(esp_vision_camera_get_width());
}
static MP_DEFINE_CONST_FUN_OBJ_0(sensor_width_obj, sensor_width);

static mp_obj_t sensor_height(void)
{
    return mp_obj_new_int_from_uint(esp_vision_camera_get_height());
}
static MP_DEFINE_CONST_FUN_OBJ_0(sensor_height_obj, sensor_height);

static mp_obj_t sensor_get_id(void)
{
    return mp_obj_new_int_from_uint(esp_vision_camera_get_sensor_id());
}
static MP_DEFINE_CONST_FUN_OBJ_0(sensor_get_id_obj, sensor_get_id);

static mp_obj_t sensor_set_hmirror(mp_obj_t enable_in)
{
    esp_err_t ret = esp_vision_camera_set_hmirror(mp_obj_is_true(enable_in));
    if (ret != ESP_OK) {
        sensor_raise_esp_err(ret);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sensor_set_hmirror_obj, sensor_set_hmirror);

static mp_obj_t sensor_get_hmirror(void)
{
    return mp_obj_new_bool(esp_vision_camera_get_hmirror());
}
static MP_DEFINE_CONST_FUN_OBJ_0(sensor_get_hmirror_obj, sensor_get_hmirror);

static mp_obj_t sensor_set_vflip(mp_obj_t enable_in)
{
    esp_err_t ret = esp_vision_camera_set_vflip(mp_obj_is_true(enable_in));
    if (ret != ESP_OK) {
        sensor_raise_esp_err(ret);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sensor_set_vflip_obj, sensor_set_vflip);

static mp_obj_t sensor_get_vflip(void)
{
    return mp_obj_new_bool(esp_vision_camera_get_vflip());
}
static MP_DEFINE_CONST_FUN_OBJ_0(sensor_get_vflip_obj, sensor_get_vflip);

static mp_obj_t sensor_skip_frames(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_time, ARG_n };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_time, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_n, MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    sensor_ensure_camera_ready();

    if ((args[ARG_time].u_int > 0) || (args[ARG_n].u_int > 0)) {
        size_t frame_size = esp_vision_camera_frame_size();
        uint8_t *pixels = sensor_get_snapshot_buf(frame_size);

        for (int i = 0; i < args[ARG_n].u_int; i++) {
            sensor_drop_frame(pixels, frame_size);
        }

        if (args[ARG_time].u_int > 0) {
            mp_uint_t start = mp_hal_ticks_ms();
            do {
                sensor_drop_frame(pixels, frame_size);
                mp_hal_delay_ms(1);
            } while ((mp_uint_t)(mp_hal_ticks_ms() - start) < (mp_uint_t)args[ARG_time].u_int);
        }
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(sensor_skip_frames_obj, 0, sensor_skip_frames);

static mp_obj_t sensor_snapshot(size_t n_args, const mp_obj_t *args)
{
    sensor_ensure_camera_ready();

    size_t frame_size = esp_vision_camera_frame_size();
    uint8_t *pixels = NULL;

    if (n_args == 0) {
        pixels = sensor_get_snapshot_buf(frame_size);
    } else {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(args[0], &bufinfo, MP_BUFFER_WRITE);
        if (bufinfo.len < frame_size) {
            mp_raise_msg_varg(&mp_type_ValueError,
                              MP_ERROR_TEXT("snapshot buffer too small: need %u"),
                              (unsigned int)frame_size);
        }
        pixels = bufinfo.buf;
    }

    esp_err_t ret = esp_vision_camera_capture(pixels, frame_size);
    if (ret != ESP_OK) {
        sensor_raise_esp_err(ret);
    }

    if (n_args == 0) {
        return esp_vision_image_new((int32_t)esp_vision_camera_get_width(),
                                    (int32_t)esp_vision_camera_get_height(),
                                    esp_vision_camera_get_pixformat(),
                                    pixels,
                                    mp_const_none);
    }
    return esp_vision_image_new((int32_t)esp_vision_camera_get_width(),
                                (int32_t)esp_vision_camera_get_height(),
                                esp_vision_camera_get_pixformat(),
                                pixels,
                                args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sensor_snapshot_obj, 0, 1, sensor_snapshot);

static mp_obj_t sensor_status(void)
{
    esp_vision_camera_status_t status;
    esp_err_t ret = esp_vision_camera_get_status(&status);
    if (ret != ESP_OK) {
        sensor_raise_esp_err(ret);
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ready), mp_obj_new_bool(status.ready));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_id), mp_obj_new_int_from_uint(status.sensor_id));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_width), mp_obj_new_int_from_uint(status.width));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_height), mp_obj_new_int_from_uint(status.height));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pixformat), mp_obj_new_int_from_uint(status.pixfmt));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hmirror), mp_obj_new_bool(status.hmirror));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_vflip), mp_obj_new_bool(status.vflip));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_raw_width), mp_obj_new_int_from_uint(status.raw_input_width));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_raw_height), mp_obj_new_int_from_uint(status.raw_input_height));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_active_width), mp_obj_new_int_from_uint(status.active_input_width));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_active_height), mp_obj_new_int_from_uint(status.active_input_height));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(sensor_status_obj, sensor_status);

static const mp_rom_map_elem_t sensor_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sensor) },

    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&sensor_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_shutdown), MP_ROM_PTR(&sensor_shutdown_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_pixformat), MP_ROM_PTR(&sensor_set_pixformat_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_pixformat), MP_ROM_PTR(&sensor_get_pixformat_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_framesize), MP_ROM_PTR(&sensor_set_framesize_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_framesize), MP_ROM_PTR(&sensor_get_framesize_obj) },
    { MP_ROM_QSTR(MP_QSTR_width), MP_ROM_PTR(&sensor_width_obj) },
    { MP_ROM_QSTR(MP_QSTR_height), MP_ROM_PTR(&sensor_height_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_id), MP_ROM_PTR(&sensor_get_id_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_hmirror), MP_ROM_PTR(&sensor_set_hmirror_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_hmirror), MP_ROM_PTR(&sensor_get_hmirror_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_vflip), MP_ROM_PTR(&sensor_set_vflip_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_vflip), MP_ROM_PTR(&sensor_get_vflip_obj) },
    { MP_ROM_QSTR(MP_QSTR_skip_frames), MP_ROM_PTR(&sensor_skip_frames_obj) },
    { MP_ROM_QSTR(MP_QSTR_snapshot), MP_ROM_PTR(&sensor_snapshot_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&sensor_status_obj) },

    { MP_ROM_QSTR(MP_QSTR_GRAYSCALE), MP_ROM_INT(SENSOR_PIXFORMAT_GRAYSCALE) },
    { MP_ROM_QSTR(MP_QSTR_RGB565), MP_ROM_INT(SENSOR_PIXFORMAT_RGB565) },
    { MP_ROM_QSTR(MP_QSTR_QQVGA), MP_ROM_INT(ESP_VISION_CAMERA_FRAMESIZE_QQVGA) },
    { MP_ROM_QSTR(MP_QSTR_QVGA), MP_ROM_INT(ESP_VISION_CAMERA_FRAMESIZE_QVGA) },
    { MP_ROM_QSTR(MP_QSTR_VGA), MP_ROM_INT(ESP_VISION_CAMERA_FRAMESIZE_VGA) },
};
static MP_DEFINE_CONST_DICT(sensor_module_globals, sensor_module_globals_table);

const mp_obj_module_t mp_module_sensor = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *) &sensor_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_sensor, mp_module_sensor);

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "py/obj.h"
#include "py/runtime.h"

#include "py_image.h"

#include "h264.h"

typedef struct py_h264_encoder_obj {
    mp_obj_base_t base;
    esp_vision_h264_enc_t *enc;
    int last_frame_type;
} py_h264_encoder_obj_t;

static void py_h264_raise_esp_err(esp_err_t ret)
{
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("h264 error: %s"), esp_err_to_name(ret));
}

static py_h264_encoder_obj_t *py_h264_encoder_obj(mp_obj_t self_in)
{
    py_h264_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->enc == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Encoder closed"));
    }
    return self;
}

static mp_obj_t py_h264_encoder_make_new(const mp_obj_type_t *type, size_t n_args,
                                         size_t n_kw, const mp_obj_t *all_args)
{
    enum { ARG_width, ARG_height, ARG_fps, ARG_gop, ARG_bitrate, ARG_qp_min, ARG_qp_max };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_width,   MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}  },
        { MP_QSTR_height,  MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}  },
        { MP_QSTR_fps,     MP_ARG_INT | MP_ARG_KW_ONLY,  {.u_int = 15} },
        { MP_QSTR_gop,     MP_ARG_INT | MP_ARG_KW_ONLY,  {.u_int = 0}  },
        { MP_QSTR_bitrate, MP_ARG_INT | MP_ARG_KW_ONLY,  {.u_int = 0}  },
        { MP_QSTR_qp_min,  MP_ARG_INT | MP_ARG_KW_ONLY,  {.u_int = 25} },
        { MP_QSTR_qp_max,  MP_ARG_INT | MP_ARG_KW_ONLY,  {.u_int = 45} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int width = args[ARG_width].u_int;
    int height = args[ARG_height].u_int;
    int fps = args[ARG_fps].u_int;
    int gop = args[ARG_gop].u_int;
    int bitrate = args[ARG_bitrate].u_int;
    int qp_min = args[ARG_qp_min].u_int;
    int qp_max = args[ARG_qp_max].u_int;

    // One intra frame per second by default.
    if (gop <= 0) {
        gop = fps;
    }
    // Roughly ~1 Mbps at QVGA/15fps; scales with resolution and frame rate.
    if (bitrate <= 0) {
        bitrate = width * height * fps;
    }

    esp_vision_h264_enc_t *enc = NULL;
    esp_err_t ret = esp_vision_h264_open(width, height, fps, gop, bitrate, qp_min, qp_max, &enc);
    if (ret != ESP_OK) {
        py_h264_raise_esp_err(ret);
    }

    py_h264_encoder_obj_t *self = mp_obj_malloc_with_finaliser(py_h264_encoder_obj_t, type);
    self->enc = enc;
    self->last_frame_type = ESP_VISION_H264_FRAME_INVALID;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t py_h264_encoder_encode(mp_obj_t self_in, mp_obj_t img_obj)
{
    py_h264_encoder_obj_t *self = py_h264_encoder_obj(self_in);
    image_t *image = py_image_cobj(img_obj);

    uint8_t *nal = NULL;
    size_t nal_len = 0;
    int frame_type = ESP_VISION_H264_FRAME_INVALID;
    esp_err_t ret = esp_vision_h264_encode(self->enc, image, &nal, &nal_len, &frame_type);
    if (ret != ESP_OK) {
        py_h264_raise_esp_err(ret);
    }

    self->last_frame_type = frame_type;
    return mp_obj_new_bytes(nal, nal_len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(py_h264_encoder_encode_obj, py_h264_encoder_encode);

static mp_obj_t py_h264_encoder_keyframe(mp_obj_t self_in)
{
    py_h264_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    bool key = (self->last_frame_type == ESP_VISION_H264_FRAME_IDR) ||
               (self->last_frame_type == ESP_VISION_H264_FRAME_I);
    return mp_obj_new_bool(key);
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_h264_encoder_keyframe_obj, py_h264_encoder_keyframe);

static mp_obj_t py_h264_encoder_close(mp_obj_t self_in)
{
    py_h264_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->enc != NULL) {
        esp_vision_h264_close(self->enc);
        self->enc = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_h264_encoder_close_obj, py_h264_encoder_close);

static const mp_rom_map_elem_t py_h264_encoder_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_encode),   MP_ROM_PTR(&py_h264_encoder_encode_obj)   },
    { MP_ROM_QSTR(MP_QSTR_keyframe), MP_ROM_PTR(&py_h264_encoder_keyframe_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),    MP_ROM_PTR(&py_h264_encoder_close_obj)    },
    { MP_ROM_QSTR(MP_QSTR___del__),  MP_ROM_PTR(&py_h264_encoder_close_obj)    },
};
static MP_DEFINE_CONST_DICT(py_h264_encoder_locals_dict, py_h264_encoder_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    py_h264_encoder_type,
    MP_QSTR_H264Encoder,
    MP_TYPE_FLAG_NONE,
    make_new, py_h264_encoder_make_new,
    locals_dict, &py_h264_encoder_locals_dict
);

static const mp_rom_map_elem_t h264_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_h264)          },
    { MP_ROM_QSTR(MP_QSTR_H264Encoder),  MP_ROM_PTR(&py_h264_encoder_type)  },
};
static MP_DEFINE_CONST_DICT(h264_module_globals, h264_module_globals_table);

const mp_obj_module_t mp_module_h264 = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *) &h264_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_h264, mp_module_h264);

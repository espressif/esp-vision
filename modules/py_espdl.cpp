/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <cstring>
#include <list>
#include <map>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dl_detect_espdet_postprocessor.hpp"
#include "dl_detect_yolo11_postprocessor.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "dl_pose_yolo11_postprocessor.hpp"
#include "esp_heap_caps.h"
#include "fbs_model.hpp"
#include "imagenet_cls_postprocessor.hpp"

extern "C" {
#include "extmod/vfs.h"
#include "py/mperrno.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py_image.h"
}

extern const mp_obj_type_t espdl_espdet_type;
extern const mp_obj_type_t espdl_yolo11_type;
extern const mp_obj_type_t espdl_yolo11n_pose_type;
extern const mp_obj_type_t espdl_imagenet_cls_type;
extern const mp_obj_type_t espdl_model_type;

typedef struct {
    uint8_t *data;
    size_t size;
} espdl_model_data_t;

typedef struct {
    mp_obj_base_t base;
    espdl_model_data_t model_data;
    dl::Model *model;
    dl::image::ImagePreprocessor *image_preprocessor;
} espdl_model_obj_t;

typedef struct {
    mp_obj_base_t base;
    espdl_model_data_t model_data;
    dl::Model *model;
    dl::image::ImagePreprocessor *image_preprocessor;
    dl::detect::ESPDetPostProcessor *postprocessor;
    float score_thr;
    float nms_thr;
} espdl_espdet_obj_t;

typedef struct {
    mp_obj_base_t base;
    espdl_model_data_t model_data;
    dl::Model *model;
    dl::image::ImagePreprocessor *image_preprocessor;
    dl::detect::yolo11PostProcessor *postprocessor;
    float score_thr;
    float nms_thr;
    int topk;
} espdl_yolo11_obj_t;

typedef struct {
    mp_obj_base_t base;
    espdl_model_data_t model_data;
    dl::Model *model;
    dl::image::ImagePreprocessor *image_preprocessor;
    dl::detect::yolo11posePostProcessor *postprocessor;
    float score_thr;
    float nms_thr;
    int topk;
} espdl_yolo11n_pose_obj_t;

typedef struct {
    mp_obj_base_t base;
    espdl_model_data_t model_data;
    dl::Model *model;
    dl::image::ImagePreprocessor *image_preprocessor;
    dl::cls::ImageNetClsPostprocessor *postprocessor;
    int topk;
    float score_thr;
} espdl_imagenet_cls_obj_t;

static void espdl_free_model_data(espdl_model_data_t *model_data)
{
    if (model_data->data == nullptr) {
        return;
    }

    heap_caps_free(model_data->data);
    model_data->data = nullptr;
    model_data->size = 0;
}

static void espdl_model_destroy(espdl_model_obj_t *self)
{
    delete self->image_preprocessor;
    self->image_preprocessor = nullptr;

    delete self->model;
    self->model = nullptr;

    espdl_free_model_data(&self->model_data);
}

static void espdl_model_ensure_active(espdl_model_obj_t *self)
{
    if ((self->model == nullptr) || (self->image_preprocessor == nullptr)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Model object is deinitialized"));
    }
}

static void espdl_espdet_destroy(espdl_espdet_obj_t *self)
{
    delete self->postprocessor;
    self->postprocessor = nullptr;

    delete self->image_preprocessor;
    self->image_preprocessor = nullptr;

    delete self->model;
    self->model = nullptr;

    espdl_free_model_data(&self->model_data);
}

static void espdl_espdet_ensure_active(espdl_espdet_obj_t *self)
{
    if ((self->model == nullptr) || (self->image_preprocessor == nullptr) || (self->postprocessor == nullptr)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("ESPDet object is deinitialized"));
    }
}

static void espdl_yolo11_destroy(espdl_yolo11_obj_t *self)
{
    delete self->postprocessor;
    self->postprocessor = nullptr;

    delete self->image_preprocessor;
    self->image_preprocessor = nullptr;

    delete self->model;
    self->model = nullptr;

    espdl_free_model_data(&self->model_data);
}

static void espdl_yolo11_ensure_active(espdl_yolo11_obj_t *self)
{
    if ((self->model == nullptr) || (self->image_preprocessor == nullptr) || (self->postprocessor == nullptr)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("YOLO11 object is deinitialized"));
    }
}

static void espdl_yolo11n_pose_destroy(espdl_yolo11n_pose_obj_t *self)
{
    delete self->postprocessor;
    self->postprocessor = nullptr;

    delete self->image_preprocessor;
    self->image_preprocessor = nullptr;

    delete self->model;
    self->model = nullptr;

    espdl_free_model_data(&self->model_data);
}

static void espdl_yolo11n_pose_ensure_active(espdl_yolo11n_pose_obj_t *self)
{
    if ((self->model == nullptr) || (self->image_preprocessor == nullptr) || (self->postprocessor == nullptr)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("YOLO11nPose object is deinitialized"));
    }
}

static void espdl_imagenet_cls_destroy(espdl_imagenet_cls_obj_t *self)
{
    delete self->postprocessor;
    self->postprocessor = nullptr;

    delete self->image_preprocessor;
    self->image_preprocessor = nullptr;

    delete self->model;
    self->model = nullptr;

    espdl_free_model_data(&self->model_data);
}

static void espdl_imagenet_cls_ensure_active(espdl_imagenet_cls_obj_t *self)
{
    if ((self->model == nullptr) || (self->image_preprocessor == nullptr) || (self->postprocessor == nullptr)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("ImageNetCls object is deinitialized"));
    }
}

static void espdl_raise_file_error(mp_obj_t file, int err)
{
    mp_stream_close(file);
    mp_raise_OSError(err);
}

static espdl_model_data_t espdl_read_model_from_vfs(mp_obj_t path_obj)
{
    mp_obj_t file_args[2] = {
        path_obj,
        MP_OBJ_NEW_QSTR(MP_QSTR_rb),
    };
    mp_obj_t file = mp_vfs_open(MP_ARRAY_SIZE(file_args), file_args, (mp_map_t *)&mp_const_empty_map);

    int err = 0;
    mp_off_t file_size = mp_stream_seek(file, 0, MP_SEEK_END, &err);
    if (file_size < 0) {
        espdl_raise_file_error(file, err);
    }
    if (mp_stream_seek(file, 0, MP_SEEK_SET, &err) < 0) {
        espdl_raise_file_error(file, err);
    }
    if (file_size <= 0) {
        mp_stream_close(file);
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("empty model file"));
    }

    const size_t size = (size_t)file_size;
    const size_t aligned_size = (size + 15U) & ~15U;
    uint8_t *data = (uint8_t *)heap_caps_aligned_alloc(16, aligned_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == nullptr) {
        data = (uint8_t *)heap_caps_aligned_alloc(16, aligned_size, MALLOC_CAP_DEFAULT);
    }
    if (data == nullptr) {
        mp_stream_close(file);
        mp_raise_msg_varg(&mp_type_MemoryError,
                          MP_ERROR_TEXT("failed to allocate %u bytes for model"),
                          (unsigned int)aligned_size);
    }

    mp_uint_t bytes_read = mp_stream_read_exactly(file, data, size, &err);
    mp_stream_close(file);
    if (err != 0) {
        heap_caps_free(data);
        mp_raise_OSError(err);
    }
    if (bytes_read != size) {
        heap_caps_free(data);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("short model read"));
    }

    espdl_model_data_t model_data = {
        .data = data,
        .size = size,
    };
    return model_data;
}

static dl::mem_info_t espdl_get_mem_info_or_zero(const std::map<std::string, dl::mem_info_t> &info, const char *key)
{
    std::map<std::string, dl::mem_info_t>::const_iterator entry = info.find(key);
    if (entry == info.end()) {
        dl::mem_info_t zero = {0, 0, 0};
        return zero;
    }
    return entry->second;
}

static void espdl_print_size(size_t size)
{
    if (size == 0) {
        mp_print_str(MP_PYTHON_PRINTER, "-");
        return;
    }

    size_t size_x100 = (size * 100 + 512) / 1024;
    mp_printf(MP_PYTHON_PRINTER,
              "%u.%02uKB",
              (unsigned int)(size_x100 / 100),
              (unsigned int)(size_x100 % 100));
}

static void espdl_print_memory_row(const char *name, const dl::mem_info_t &info)
{
    mp_printf(MP_PYTHON_PRINTER, "%-16s internal=", name);
    espdl_print_size(info.internal);
    mp_print_str(MP_PYTHON_PRINTER, " psram=");
    espdl_print_size(info.psram);
    mp_print_str(MP_PYTHON_PRINTER, " flash=");
    espdl_print_size(info.flash);
    mp_print_str(MP_PYTHON_PRINTER, "\n");
}

static void espdl_print_profile(dl::Model *model)
{
    mp_print_str(MP_PYTHON_PRINTER, "\nESP-DL memory summary\n");
    std::map<std::string, dl::mem_info_t> mem_info = model->get_memory_info();
    espdl_print_memory_row("fbs_model", espdl_get_mem_info_or_zero(mem_info, "fbs_model"));
    espdl_print_memory_row("parameter", espdl_get_mem_info_or_zero(mem_info, "parameter"));
    espdl_print_memory_row("parameter_copy", espdl_get_mem_info_or_zero(mem_info, "parameter_copy"));
    espdl_print_memory_row("variable", espdl_get_mem_info_or_zero(mem_info, "variable"));
    espdl_print_memory_row("others", espdl_get_mem_info_or_zero(mem_info, "others"));
    espdl_print_memory_row("total", espdl_get_mem_info_or_zero(mem_info, "total"));

    mp_print_str(MP_PYTHON_PRINTER, "\nESP-DL module summary\n");
    std::map<std::string, dl::module_info> module_info = model->get_module_info();
    std::vector<std::pair<std::string, dl::module_info>> modules;
    modules.reserve(module_info.size());
    for (const std::pair<const std::string, dl::module_info> &entry : module_info) {
        if (entry.first != "total") {
            modules.push_back(entry);
        }
    }

    mp_printf(MP_PYTHON_PRINTER, "%-32s %-16s %s\n", "name", "type", "latency");
    for (const std::pair<std::string, dl::module_info> &entry : modules) {
        mp_printf(MP_PYTHON_PRINTER,
                  "%-32s %-16s %uus\n",
                  entry.first.c_str(),
                  entry.second.type.c_str(),
                  (unsigned int)entry.second.latency);
    }

    std::map<std::string, dl::module_info>::const_iterator total = module_info.find("total");
    if (total != module_info.end()) {
        mp_printf(MP_PYTHON_PRINTER,
                  "%-32s %-16s %uus\n",
                  total->first.c_str(),
                  total->second.type.c_str(),
                  (unsigned int)total->second.latency);
    }
    mp_print_str(MP_PYTHON_PRINTER, "\n");
}

static mp_obj_t espdl_load_model(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_path,
        ARG_profile,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_profile, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    espdl_model_data_t model_data = espdl_read_model_from_vfs(args[ARG_path].u_obj);
    dl::Model *model = new (std::nothrow) dl::Model((const char *)model_data.data,
                                                    fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (model == nullptr) {
        espdl_free_model_data(&model_data);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate model object"));
    }
    if (model->get_fbs_model() == nullptr) {
        delete model;
        espdl_free_model_data(&model_data);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("model load failed"));
    }

    if (args[ARG_profile].u_bool) {
        espdl_print_profile(model);
    }

    delete model;
    espdl_free_model_data(&model_data);
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espdl_load_model_obj, 1, espdl_load_model);

static float espdl_arg_get_float_or_default(mp_obj_t obj, float default_value)
{
    return (obj == MP_OBJ_NULL) ? default_value : mp_obj_get_float_to_f(obj);
}

static std::array<float, 3> espdl_arg_get_float3_or_default(
    mp_obj_t obj,
    const std::array<float, 3> &default_value)
{
    if (obj == MP_OBJ_NULL) {
        return default_value;
    }

    if (mp_obj_is_type(obj, &mp_type_tuple) || mp_obj_is_type(obj, &mp_type_list)) {
        mp_obj_t *items = nullptr;
        mp_obj_get_array_fixed_n(obj, 3, &items);
        return {
            mp_obj_get_float_to_f(items[0]),
            mp_obj_get_float_to_f(items[1]),
            mp_obj_get_float_to_f(items[2]),
        };
    }

    float value = mp_obj_get_float_to_f(obj);
    return {value, value, value};
}

static void espdl_validate_preprocess_std(const std::array<float, 3> &std_values)
{
    if ((std_values[0] <= 0.0f) || (std_values[1] <= 0.0f) || (std_values[2] <= 0.0f)) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("std values must be positive"));
    }
}

static uint8_t espdl_clamp_u8(float value)
{
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 255.0f) {
        return 255;
    }
    return (uint8_t)value;
}

static int espdl_validate_topk(mp_int_t topk)
{
    if (topk < 1) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("topk must be >= 1"));
    }
    return (int)topk;
}

static image_t *espdl_get_image(mp_obj_t img_in)
{
    if (!mp_obj_is_type(img_in, &py_image_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected image.Image"));
    }

    image_t *img = (image_t *)py_image_cobj(img_in);
    if ((img == nullptr) || (img->pixels == nullptr) || (img->w <= 0) || (img->h <= 0)) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("invalid image"));
    }
    return img;
}

static dl::image::pix_type_t espdl_image_pix_type(image_t *img)
{
    if (img->pixfmt == PIXFORMAT_RGB565) {
        return dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
    }
    if (img->pixfmt == PIXFORMAT_GRAYSCALE) {
        return dl::image::DL_IMAGE_PIX_TYPE_GRAY;
    }

    mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("ESP-DL only supports RGB565 or grayscale images"));
}

static dl::image::img_t espdl_make_dl_image(image_t *img)
{
    dl::image::img_t dl_img;
    dl_img.data = img->pixels;
    dl_img.width = (uint16_t)img->w;
    dl_img.height = (uint16_t)img->h;
    dl_img.pix_type = espdl_image_pix_type(img);
    return dl_img;
}

static std::vector<dl::detect::anchor_point_stage_t> espdl_make_anchor_point_stages(void)
{
    const dl::detect::anchor_point_stage_t stage_8 = {8, 8, 4, 4};
    const dl::detect::anchor_point_stage_t stage_16 = {16, 16, 8, 8};
    const dl::detect::anchor_point_stage_t stage_32 = {32, 32, 16, 16};

    return {
        stage_8,
        stage_16,
        stage_32,
    };
}

static mp_obj_t espdl_tensor_shape_to_tuple(dl::TensorBase *tensor)
{
    std::vector<mp_obj_t> shape;
    shape.reserve(tensor->shape.size());
    for (int dim : tensor->shape) {
        shape.push_back(mp_obj_new_int(dim));
    }
    return mp_obj_new_tuple(shape.size(), shape.data());
}

static mp_obj_t espdl_tensor_info_to_tuple(const std::string &name, dl::TensorBase *tensor, bool with_data)
{
    mp_obj_t items[5];
    items[0] = mp_obj_new_str(name.c_str(), name.size());
    items[1] = espdl_tensor_shape_to_tuple(tensor);
    items[2] = mp_obj_new_int((int)tensor->dtype);
    items[3] = mp_obj_new_int(tensor->get_exponent());
    if (with_data) {
        items[4] = mp_obj_new_bytes((const byte *)tensor->get_element_ptr(), tensor->get_bytes());
    }
    return mp_obj_new_tuple(with_data ? 5 : 4, items);
}

static mp_obj_t espdl_tensor_map_info_to_dict(std::map<std::string, dl::TensorBase *> &tensors, bool with_data)
{
    mp_obj_t dict = mp_obj_new_dict(tensors.size());
    for (const std::pair<const std::string, dl::TensorBase *> &entry : tensors) {
        mp_obj_dict_store(
            dict,
            mp_obj_new_str(entry.first.c_str(), entry.first.size()),
            espdl_tensor_info_to_tuple(entry.first, entry.second, with_data));
    }
    return dict;
}

static mp_obj_t espdl_model_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args)
{
    enum {
        ARG_path,
        ARG_mean,
        ARG_std,
        ARG_letterbox,
        ARG_pad,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_mean, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_std, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_letterbox, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
        {MP_QSTR_pad, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    std::array<float, 3> mean_values = espdl_arg_get_float3_or_default(args[ARG_mean].u_obj, {0.0f, 0.0f, 0.0f});
    std::array<float, 3> std_values =
        espdl_arg_get_float3_or_default(args[ARG_std].u_obj, {255.0f, 255.0f, 255.0f});
    std::array<float, 3> pad_values = espdl_arg_get_float3_or_default(args[ARG_pad].u_obj, {114.0f, 114.0f, 114.0f});
    espdl_validate_preprocess_std(std_values);

    espdl_model_obj_t *self = mp_obj_malloc_with_finaliser(espdl_model_obj_t, type);
    self->base.type = type;
    self->model_data.data = nullptr;
    self->model_data.size = 0;
    self->model = nullptr;
    self->image_preprocessor = nullptr;

    self->model_data = espdl_read_model_from_vfs(args[ARG_path].u_obj);
    self->model = new (std::nothrow) dl::Model((const char *)self->model_data.data,
                                               fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (self->model == nullptr) {
        espdl_model_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate model object"));
    }
    if (self->model->get_fbs_model() == nullptr) {
        espdl_model_destroy(self);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("model load failed"));
    }

    self->model->minimize();
    self->image_preprocessor =
        new (std::nothrow) dl::image::ImagePreprocessor(self->model, mean_values, std_values);
    if (self->image_preprocessor == nullptr) {
        espdl_model_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate image preprocessor"));
    }
    if (args[ARG_letterbox].u_bool) {
        self->image_preprocessor->enable_letterbox({
            espdl_clamp_u8(pad_values[0]),
            espdl_clamp_u8(pad_values[1]),
            espdl_clamp_u8(pad_values[2]),
        });
    }

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t espdl_model_predict(mp_obj_t self_in, mp_obj_t img_in)
{
    espdl_model_obj_t *self = (espdl_model_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_model_ensure_active(self);

    image_t *img = espdl_get_image(img_in);
    dl::image::img_t dl_img = espdl_make_dl_image(img);

    self->image_preprocessor->preprocess(dl_img);
    self->model->run();

    return espdl_tensor_map_info_to_dict(self->model->get_outputs(), true);
}
static MP_DEFINE_CONST_FUN_OBJ_2(espdl_model_predict_obj, espdl_model_predict);

static mp_obj_t espdl_model_inputs(mp_obj_t self_in)
{
    espdl_model_obj_t *self = (espdl_model_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_model_ensure_active(self);

    return espdl_tensor_map_info_to_dict(self->model->get_inputs(), false);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espdl_model_inputs_obj, espdl_model_inputs);

static mp_obj_t espdl_model_outputs(mp_obj_t self_in)
{
    espdl_model_obj_t *self = (espdl_model_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_model_ensure_active(self);

    return espdl_tensor_map_info_to_dict(self->model->get_outputs(), false);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espdl_model_outputs_obj, espdl_model_outputs);

static mp_obj_t espdl_model_deinit(mp_obj_t self_in)
{
    espdl_model_obj_t *self = (espdl_model_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_model_destroy(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espdl_model_deinit_obj, espdl_model_deinit);

static const mp_rom_map_elem_t espdl_model_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espdl_model_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espdl_model_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_inputs), MP_ROM_PTR(&espdl_model_inputs_obj)},
    {MP_ROM_QSTR(MP_QSTR_outputs), MP_ROM_PTR(&espdl_model_outputs_obj)},
    {MP_ROM_QSTR(MP_QSTR_predict), MP_ROM_PTR(&espdl_model_predict_obj)},
};
static MP_DEFINE_CONST_DICT(espdl_model_locals_dict, espdl_model_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espdl_model_type,
    MP_QSTR_Model,
    MP_TYPE_FLAG_NONE,
    make_new, reinterpret_cast<const void *>(espdl_model_make_new),
    locals_dict, &espdl_model_locals_dict
);

static mp_obj_t espdl_detect_results_to_list(std::list<dl::detect::result_t> &results)
{
    mp_obj_t detections = mp_obj_new_list(0, nullptr);
    for (const dl::detect::result_t &result : results) {
        if (result.box.size() < 4) {
            continue;
        }

        int x = result.box[0];
        int y = result.box[1];
        int w = result.box[2] - result.box[0] + 1;
        int h = result.box[3] - result.box[1] + 1;
        if ((w <= 0) || (h <= 0)) {
            continue;
        }

        mp_obj_t tuple[6] = {
            mp_obj_new_int(x),
            mp_obj_new_int(y),
            mp_obj_new_int(w),
            mp_obj_new_int(h),
            mp_obj_new_float(result.score),
            mp_obj_new_int(result.category),
        };
        mp_obj_list_append(detections, mp_obj_new_tuple(MP_ARRAY_SIZE(tuple), tuple));
    }

    return detections;
}

static mp_obj_t espdl_pose_results_to_list(std::list<dl::detect::result_t> &results)
{
    mp_obj_t detections = mp_obj_new_list(0, nullptr);
    for (const dl::detect::result_t &result : results) {
        if (result.box.size() < 4) {
            continue;
        }

        int x = result.box[0];
        int y = result.box[1];
        int w = result.box[2] - result.box[0] + 1;
        int h = result.box[3] - result.box[1] + 1;
        if ((w <= 0) || (h <= 0)) {
            continue;
        }

        mp_obj_t keypoints = mp_obj_new_list(0, nullptr);
        for (size_t i = 0; i + 1 < result.keypoint.size(); i += 2) {
            mp_obj_t keypoint[2] = {
                mp_obj_new_int(result.keypoint[i]),
                mp_obj_new_int(result.keypoint[i + 1]),
            };
            mp_obj_list_append(keypoints, mp_obj_new_tuple(MP_ARRAY_SIZE(keypoint), keypoint));
        }

        mp_obj_t tuple[7] = {
            mp_obj_new_int(x),
            mp_obj_new_int(y),
            mp_obj_new_int(w),
            mp_obj_new_int(h),
            mp_obj_new_float(result.score),
            mp_obj_new_int(result.category),
            keypoints,
        };
        mp_obj_list_append(detections, mp_obj_new_tuple(MP_ARRAY_SIZE(tuple), tuple));
    }

    return detections;
}

static mp_obj_t espdl_espdet_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args)
{
    enum {
        ARG_path,
        ARG_score,
        ARG_nms,
        ARG_mean,
        ARG_std,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_mean, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_std, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    float score_thr = espdl_arg_get_float_or_default(args[ARG_score].u_obj, 0.5f);
    float nms_thr = espdl_arg_get_float_or_default(args[ARG_nms].u_obj, 0.7f);
    std::array<float, 3> mean_values = espdl_arg_get_float3_or_default(args[ARG_mean].u_obj, {0.0f, 0.0f, 0.0f});
    std::array<float, 3> std_values =
        espdl_arg_get_float3_or_default(args[ARG_std].u_obj, {255.0f, 255.0f, 255.0f});
    espdl_validate_preprocess_std(std_values);

    espdl_espdet_obj_t *self = mp_obj_malloc_with_finaliser(espdl_espdet_obj_t, type);
    self->base.type = type;
    self->model_data.data = nullptr;
    self->model_data.size = 0;
    self->model = nullptr;
    self->image_preprocessor = nullptr;
    self->postprocessor = nullptr;
    self->score_thr = score_thr;
    self->nms_thr = nms_thr;

    self->model_data = espdl_read_model_from_vfs(args[ARG_path].u_obj);
    self->model = new (std::nothrow) dl::Model((const char *)self->model_data.data,
                                               fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (self->model == nullptr) {
        espdl_espdet_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate model object"));
    }
    if (self->model->get_fbs_model() == nullptr) {
        espdl_espdet_destroy(self);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("model load failed"));
    }

    self->model->minimize();
    self->image_preprocessor =
        new (std::nothrow) dl::image::ImagePreprocessor(self->model, mean_values, std_values);
    if (self->image_preprocessor == nullptr) {
        espdl_espdet_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate image preprocessor"));
    }
    self->image_preprocessor->enable_letterbox({114, 114, 114});

    std::vector<dl::detect::anchor_point_stage_t> espdet_stages = espdl_make_anchor_point_stages();

    self->postprocessor = new (std::nothrow) dl::detect::ESPDetPostProcessor(
        self->model,
        self->image_preprocessor,
        self->score_thr,
        self->nms_thr,
        10,
        espdet_stages);
    if (self->postprocessor == nullptr) {
        espdl_espdet_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate ESPDet postprocessor"));
    }

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t espdl_espdet_detect(mp_obj_t self_in, mp_obj_t img_in)
{
    espdl_espdet_obj_t *self = (espdl_espdet_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_espdet_ensure_active(self);

    image_t *img = espdl_get_image(img_in);
    dl::image::img_t dl_img = espdl_make_dl_image(img);

    self->image_preprocessor->preprocess(dl_img);
    self->model->run();
    self->postprocessor->clear_result();
    self->postprocessor->postprocess();

    std::list<dl::detect::result_t> &results = self->postprocessor->get_result(img->w, img->h);
    return espdl_detect_results_to_list(results);
}
static MP_DEFINE_CONST_FUN_OBJ_2(espdl_espdet_detect_obj, espdl_espdet_detect);

static mp_obj_t espdl_espdet_set_thresholds(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_self,
        ARG_score,
        ARG_nms,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    espdl_espdet_obj_t *self = (espdl_espdet_obj_t *)MP_OBJ_TO_PTR(args[ARG_self].u_obj);
    espdl_espdet_ensure_active(self);

    if (args[ARG_score].u_obj != MP_OBJ_NULL) {
        self->score_thr = mp_obj_get_float_to_f(args[ARG_score].u_obj);
        self->postprocessor->set_score_thr(self->score_thr);
    }
    if (args[ARG_nms].u_obj != MP_OBJ_NULL) {
        self->nms_thr = mp_obj_get_float_to_f(args[ARG_nms].u_obj);
        self->postprocessor->set_nms_thr(self->nms_thr);
    }

    return args[ARG_self].u_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espdl_espdet_set_thresholds_obj, 1, espdl_espdet_set_thresholds);

static mp_obj_t espdl_espdet_deinit(mp_obj_t self_in)
{
    espdl_espdet_obj_t *self = (espdl_espdet_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_espdet_destroy(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espdl_espdet_deinit_obj, espdl_espdet_deinit);

static const mp_rom_map_elem_t espdl_espdet_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espdl_espdet_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espdl_espdet_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_detect), MP_ROM_PTR(&espdl_espdet_detect_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_thresholds), MP_ROM_PTR(&espdl_espdet_set_thresholds_obj)},
};
static MP_DEFINE_CONST_DICT(espdl_espdet_locals_dict, espdl_espdet_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espdl_espdet_type,
    MP_QSTR_ESPDet,
    MP_TYPE_FLAG_NONE,
    make_new, reinterpret_cast<const void *>(espdl_espdet_make_new),
    locals_dict, &espdl_espdet_locals_dict
);

static mp_obj_t espdl_yolo11_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args)
{
    enum {
        ARG_path,
        ARG_score,
        ARG_nms,
        ARG_topk,
        ARG_mean,
        ARG_std,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_topk, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 10}},
        {MP_QSTR_mean, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_std, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    float score_thr = espdl_arg_get_float_or_default(args[ARG_score].u_obj, 0.25f);
    float nms_thr = espdl_arg_get_float_or_default(args[ARG_nms].u_obj, 0.7f);
    int topk = espdl_validate_topk(args[ARG_topk].u_int);
    std::array<float, 3> mean_values = espdl_arg_get_float3_or_default(args[ARG_mean].u_obj, {0.0f, 0.0f, 0.0f});
    std::array<float, 3> std_values =
        espdl_arg_get_float3_or_default(args[ARG_std].u_obj, {255.0f, 255.0f, 255.0f});
    espdl_validate_preprocess_std(std_values);

    espdl_yolo11_obj_t *self = mp_obj_malloc_with_finaliser(espdl_yolo11_obj_t, type);
    self->base.type = type;
    self->model_data.data = nullptr;
    self->model_data.size = 0;
    self->model = nullptr;
    self->image_preprocessor = nullptr;
    self->postprocessor = nullptr;
    self->score_thr = score_thr;
    self->nms_thr = nms_thr;
    self->topk = topk;

    self->model_data = espdl_read_model_from_vfs(args[ARG_path].u_obj);
    self->model = new (std::nothrow) dl::Model((const char *)self->model_data.data,
                                               fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (self->model == nullptr) {
        espdl_yolo11_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate model object"));
    }
    if (self->model->get_fbs_model() == nullptr) {
        espdl_yolo11_destroy(self);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("model load failed"));
    }

    self->model->minimize();
    self->image_preprocessor =
        new (std::nothrow) dl::image::ImagePreprocessor(self->model, mean_values, std_values);
    if (self->image_preprocessor == nullptr) {
        espdl_yolo11_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate image preprocessor"));
    }
    self->image_preprocessor->enable_letterbox({114, 114, 114});

    std::vector<dl::detect::anchor_point_stage_t> yolo11_stages = espdl_make_anchor_point_stages();

    self->postprocessor = new (std::nothrow) dl::detect::yolo11PostProcessor(
        self->model,
        self->image_preprocessor,
        self->score_thr,
        self->nms_thr,
        self->topk,
        yolo11_stages);
    if (self->postprocessor == nullptr) {
        espdl_yolo11_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate YOLO11 postprocessor"));
    }

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t espdl_yolo11_detect(mp_obj_t self_in, mp_obj_t img_in)
{
    espdl_yolo11_obj_t *self = (espdl_yolo11_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_yolo11_ensure_active(self);

    image_t *img = espdl_get_image(img_in);
    dl::image::img_t dl_img = espdl_make_dl_image(img);

    self->image_preprocessor->preprocess(dl_img);
    self->model->run();
    self->postprocessor->clear_result();
    self->postprocessor->postprocess();

    std::list<dl::detect::result_t> &results = self->postprocessor->get_result(img->w, img->h);
    return espdl_detect_results_to_list(results);
}
static MP_DEFINE_CONST_FUN_OBJ_2(espdl_yolo11_detect_obj, espdl_yolo11_detect);

static mp_obj_t espdl_yolo11_set_thresholds(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_self,
        ARG_score,
        ARG_nms,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    espdl_yolo11_obj_t *self = (espdl_yolo11_obj_t *)MP_OBJ_TO_PTR(args[ARG_self].u_obj);
    espdl_yolo11_ensure_active(self);

    if (args[ARG_score].u_obj != MP_OBJ_NULL) {
        self->score_thr = mp_obj_get_float_to_f(args[ARG_score].u_obj);
        self->postprocessor->set_score_thr(self->score_thr);
    }
    if (args[ARG_nms].u_obj != MP_OBJ_NULL) {
        self->nms_thr = mp_obj_get_float_to_f(args[ARG_nms].u_obj);
        self->postprocessor->set_nms_thr(self->nms_thr);
    }

    return args[ARG_self].u_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espdl_yolo11_set_thresholds_obj, 1, espdl_yolo11_set_thresholds);

static mp_obj_t espdl_yolo11_deinit(mp_obj_t self_in)
{
    espdl_yolo11_obj_t *self = (espdl_yolo11_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_yolo11_destroy(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espdl_yolo11_deinit_obj, espdl_yolo11_deinit);

static const mp_rom_map_elem_t espdl_yolo11_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espdl_yolo11_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espdl_yolo11_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_detect), MP_ROM_PTR(&espdl_yolo11_detect_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_thresholds), MP_ROM_PTR(&espdl_yolo11_set_thresholds_obj)},
};
static MP_DEFINE_CONST_DICT(espdl_yolo11_locals_dict, espdl_yolo11_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espdl_yolo11_type,
    MP_QSTR_YOLO11,
    MP_TYPE_FLAG_NONE,
    make_new, reinterpret_cast<const void *>(espdl_yolo11_make_new),
    locals_dict, &espdl_yolo11_locals_dict
);

static mp_obj_t espdl_yolo11n_pose_make_new(
    const mp_obj_type_t *type,
    size_t n_args,
    size_t n_kw,
    const mp_obj_t *all_args)
{
    enum {
        ARG_path,
        ARG_score,
        ARG_nms,
        ARG_topk,
        ARG_mean,
        ARG_std,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_topk, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 10}},
        {MP_QSTR_mean, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_std, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    float score_thr = espdl_arg_get_float_or_default(args[ARG_score].u_obj, 0.25f);
    float nms_thr = espdl_arg_get_float_or_default(args[ARG_nms].u_obj, 0.7f);
    int topk = espdl_validate_topk(args[ARG_topk].u_int);
    std::array<float, 3> mean_values = espdl_arg_get_float3_or_default(args[ARG_mean].u_obj, {0.0f, 0.0f, 0.0f});
    std::array<float, 3> std_values =
        espdl_arg_get_float3_or_default(args[ARG_std].u_obj, {255.0f, 255.0f, 255.0f});
    espdl_validate_preprocess_std(std_values);

    espdl_yolo11n_pose_obj_t *self = mp_obj_malloc_with_finaliser(espdl_yolo11n_pose_obj_t, type);
    self->base.type = type;
    self->model_data.data = nullptr;
    self->model_data.size = 0;
    self->model = nullptr;
    self->image_preprocessor = nullptr;
    self->postprocessor = nullptr;
    self->score_thr = score_thr;
    self->nms_thr = nms_thr;
    self->topk = topk;

    self->model_data = espdl_read_model_from_vfs(args[ARG_path].u_obj);
    self->model = new (std::nothrow) dl::Model((const char *)self->model_data.data,
                                               fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (self->model == nullptr) {
        espdl_yolo11n_pose_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate model object"));
    }
    if (self->model->get_fbs_model() == nullptr) {
        espdl_yolo11n_pose_destroy(self);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("model load failed"));
    }

    self->model->minimize();
    self->image_preprocessor =
        new (std::nothrow) dl::image::ImagePreprocessor(self->model, mean_values, std_values);
    if (self->image_preprocessor == nullptr) {
        espdl_yolo11n_pose_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate image preprocessor"));
    }
    self->image_preprocessor->enable_letterbox({114, 114, 114});

    std::vector<dl::detect::anchor_point_stage_t> yolo11_stages = espdl_make_anchor_point_stages();

    self->postprocessor = new (std::nothrow) dl::detect::yolo11posePostProcessor(
        self->model,
        self->image_preprocessor,
        self->score_thr,
        self->nms_thr,
        self->topk,
        yolo11_stages);
    if (self->postprocessor == nullptr) {
        espdl_yolo11n_pose_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate YOLO11nPose postprocessor"));
    }

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t espdl_yolo11n_pose_detect(mp_obj_t self_in, mp_obj_t img_in)
{
    espdl_yolo11n_pose_obj_t *self = (espdl_yolo11n_pose_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_yolo11n_pose_ensure_active(self);

    image_t *img = espdl_get_image(img_in);
    dl::image::img_t dl_img = espdl_make_dl_image(img);

    self->image_preprocessor->preprocess(dl_img);
    self->model->run();
    self->postprocessor->clear_result();
    self->postprocessor->postprocess();

    std::list<dl::detect::result_t> &results = self->postprocessor->get_result(img->w, img->h);
    return espdl_pose_results_to_list(results);
}
static MP_DEFINE_CONST_FUN_OBJ_2(espdl_yolo11n_pose_detect_obj, espdl_yolo11n_pose_detect);

static mp_obj_t espdl_yolo11n_pose_set_thresholds(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_self,
        ARG_score,
        ARG_nms,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    espdl_yolo11n_pose_obj_t *self = (espdl_yolo11n_pose_obj_t *)MP_OBJ_TO_PTR(args[ARG_self].u_obj);
    espdl_yolo11n_pose_ensure_active(self);

    if (args[ARG_score].u_obj != MP_OBJ_NULL) {
        self->score_thr = mp_obj_get_float_to_f(args[ARG_score].u_obj);
        self->postprocessor->set_score_thr(self->score_thr);
    }
    if (args[ARG_nms].u_obj != MP_OBJ_NULL) {
        self->nms_thr = mp_obj_get_float_to_f(args[ARG_nms].u_obj);
        self->postprocessor->set_nms_thr(self->nms_thr);
    }

    return args[ARG_self].u_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espdl_yolo11n_pose_set_thresholds_obj, 1, espdl_yolo11n_pose_set_thresholds);

static mp_obj_t espdl_yolo11n_pose_deinit(mp_obj_t self_in)
{
    espdl_yolo11n_pose_obj_t *self = (espdl_yolo11n_pose_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_yolo11n_pose_destroy(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espdl_yolo11n_pose_deinit_obj, espdl_yolo11n_pose_deinit);

static const mp_rom_map_elem_t espdl_yolo11n_pose_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espdl_yolo11n_pose_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espdl_yolo11n_pose_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_detect), MP_ROM_PTR(&espdl_yolo11n_pose_detect_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_thresholds), MP_ROM_PTR(&espdl_yolo11n_pose_set_thresholds_obj)},
};
static MP_DEFINE_CONST_DICT(espdl_yolo11n_pose_locals_dict, espdl_yolo11n_pose_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espdl_yolo11n_pose_type,
    MP_QSTR_YOLO11nPose,
    MP_TYPE_FLAG_NONE,
    make_new, reinterpret_cast<const void *>(espdl_yolo11n_pose_make_new),
    locals_dict, &espdl_yolo11n_pose_locals_dict
);

static mp_obj_t espdl_imagenet_cls_make_new(
    const mp_obj_type_t *type,
    size_t n_args,
    size_t n_kw,
    const mp_obj_t *all_args)
{
    enum {
        ARG_path,
        ARG_topk,
        ARG_score,
        ARG_mean,
        ARG_std,
        ARG_softmax,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_topk, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 5}},
        {MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_mean, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_std, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_softmax, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int topk = espdl_validate_topk(args[ARG_topk].u_int);
    float score_thr = espdl_arg_get_float_or_default(args[ARG_score].u_obj, 0.0f);
    std::array<float, 3> mean_values =
        espdl_arg_get_float3_or_default(args[ARG_mean].u_obj, {123.675f, 116.28f, 103.53f});
    std::array<float, 3> std_values =
        espdl_arg_get_float3_or_default(args[ARG_std].u_obj, {58.395f, 57.12f, 57.375f});
    espdl_validate_preprocess_std(std_values);

    espdl_imagenet_cls_obj_t *self = mp_obj_malloc_with_finaliser(espdl_imagenet_cls_obj_t, type);
    self->base.type = type;
    self->model_data.data = nullptr;
    self->model_data.size = 0;
    self->model = nullptr;
    self->image_preprocessor = nullptr;
    self->postprocessor = nullptr;
    self->topk = topk;
    self->score_thr = score_thr;

    self->model_data = espdl_read_model_from_vfs(args[ARG_path].u_obj);
    self->model = new (std::nothrow) dl::Model((const char *)self->model_data.data,
                                               fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (self->model == nullptr) {
        espdl_imagenet_cls_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate model object"));
    }
    if (self->model->get_fbs_model() == nullptr) {
        espdl_imagenet_cls_destroy(self);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("model load failed"));
    }

    self->model->minimize();
    self->image_preprocessor =
        new (std::nothrow) dl::image::ImagePreprocessor(self->model, mean_values, std_values);
    if (self->image_preprocessor == nullptr) {
        espdl_imagenet_cls_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate image preprocessor"));
    }

    self->postprocessor = new (std::nothrow) dl::cls::ImageNetClsPostprocessor(
        self->model,
        self->topk,
        self->score_thr,
        args[ARG_softmax].u_bool);
    if (self->postprocessor == nullptr) {
        espdl_imagenet_cls_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate ImageNetCls postprocessor"));
    }

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t espdl_imagenet_cls_classify(mp_obj_t self_in, mp_obj_t img_in)
{
    espdl_imagenet_cls_obj_t *self = (espdl_imagenet_cls_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_imagenet_cls_ensure_active(self);

    image_t *img = espdl_get_image(img_in);
    dl::image::img_t dl_img = espdl_make_dl_image(img);

    self->image_preprocessor->preprocess(dl_img);
    self->model->run();

    std::vector<dl::cls::result_t> &results = self->postprocessor->postprocess();
    mp_obj_t classifications = mp_obj_new_list(0, nullptr);
    for (const dl::cls::result_t &result : results) {
        const char *cat_name = result.cat_name == nullptr ? "" : result.cat_name;
        mp_obj_t tuple[2] = {
            mp_obj_new_str(cat_name, std::strlen(cat_name)),
            mp_obj_new_float(result.score),
        };
        mp_obj_list_append(classifications, mp_obj_new_tuple(MP_ARRAY_SIZE(tuple), tuple));
    }

    return classifications;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espdl_imagenet_cls_classify_obj, espdl_imagenet_cls_classify);

static mp_obj_t espdl_imagenet_cls_set_thresholds(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_self,
        ARG_topk,
        ARG_score,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_topk, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    espdl_imagenet_cls_obj_t *self = (espdl_imagenet_cls_obj_t *)MP_OBJ_TO_PTR(args[ARG_self].u_obj);
    espdl_imagenet_cls_ensure_active(self);

    if (args[ARG_topk].u_obj != MP_OBJ_NULL) {
        self->topk = espdl_validate_topk(mp_obj_get_int(args[ARG_topk].u_obj));
        self->postprocessor->set_topk(self->topk);
    }
    if (args[ARG_score].u_obj != MP_OBJ_NULL) {
        self->score_thr = mp_obj_get_float_to_f(args[ARG_score].u_obj);
        self->postprocessor->set_score_thr(self->score_thr);
    }

    return args[ARG_self].u_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espdl_imagenet_cls_set_thresholds_obj, 1, espdl_imagenet_cls_set_thresholds);

static mp_obj_t espdl_imagenet_cls_deinit(mp_obj_t self_in)
{
    espdl_imagenet_cls_obj_t *self = (espdl_imagenet_cls_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_imagenet_cls_destroy(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espdl_imagenet_cls_deinit_obj, espdl_imagenet_cls_deinit);

static const mp_rom_map_elem_t espdl_imagenet_cls_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espdl_imagenet_cls_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_classify), MP_ROM_PTR(&espdl_imagenet_cls_classify_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espdl_imagenet_cls_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_thresholds), MP_ROM_PTR(&espdl_imagenet_cls_set_thresholds_obj)},
};
static MP_DEFINE_CONST_DICT(espdl_imagenet_cls_locals_dict, espdl_imagenet_cls_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espdl_imagenet_cls_type,
    MP_QSTR_ImageNetCls,
    MP_TYPE_FLAG_NONE,
    make_new, reinterpret_cast<const void *>(espdl_imagenet_cls_make_new),
    locals_dict, &espdl_imagenet_cls_locals_dict
);

static const mp_rom_map_elem_t espdl_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_espdl)},
    {MP_ROM_QSTR(MP_QSTR_Model), MP_ROM_PTR(&espdl_model_type)},
    {MP_ROM_QSTR(MP_QSTR_ESPDet), MP_ROM_PTR(&espdl_espdet_type)},
    {MP_ROM_QSTR(MP_QSTR_YOLO11), MP_ROM_PTR(&espdl_yolo11_type)},
    {MP_ROM_QSTR(MP_QSTR_YOLO11nPose), MP_ROM_PTR(&espdl_yolo11n_pose_type)},
    {MP_ROM_QSTR(MP_QSTR_ImageNetCls), MP_ROM_PTR(&espdl_imagenet_cls_type)},
    {MP_ROM_QSTR(MP_QSTR_load_model), MP_ROM_PTR(&espdl_load_model_obj)},
};
static MP_DEFINE_CONST_DICT(espdl_module_globals, espdl_module_globals_table);

extern "C" const mp_obj_module_t espdl_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *) &espdl_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_espdl, espdl_module);

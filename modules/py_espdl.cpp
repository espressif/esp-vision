/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

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
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "esp_heap_caps.h"
#include "fbs_model.hpp"

extern "C" {
#include "extmod/vfs.h"
#include "py/mperrno.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py_image.h"
}

extern const mp_obj_type_t espdl_espdet_type;

typedef struct {
    uint8_t *data;
    size_t size;
} espdl_model_data_t;

typedef struct {
    mp_obj_base_t base;
    espdl_model_data_t model_data;
    dl::Model *model;
    dl::image::ImagePreprocessor *image_preprocessor;
    dl::detect::ESPDetPostProcessor *postprocessor;
    float score_thr;
    float nms_thr;
} espdl_espdet_obj_t;

static void espdl_free_model_data(espdl_model_data_t *model_data)
{
    if (model_data->data == nullptr) {
        return;
    }

    heap_caps_free(model_data->data);
    model_data->data = nullptr;
    model_data->size = 0;
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

static mp_obj_t espdl_espdet_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args)
{
    enum {
        ARG_path,
        ARG_score,
        ARG_nms,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    float score_thr = espdl_arg_get_float_or_default(args[ARG_score].u_obj, 0.5f);
    float nms_thr = espdl_arg_get_float_or_default(args[ARG_nms].u_obj, 0.7f);

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
        new (std::nothrow) dl::image::ImagePreprocessor(self->model, {0, 0, 0}, {255, 255, 255});
    if (self->image_preprocessor == nullptr) {
        espdl_espdet_destroy(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("failed to allocate image preprocessor"));
    }
    self->image_preprocessor->enable_letterbox({114, 114, 114});

    const dl::detect::anchor_point_stage_t espdet_stage_8 = {8, 8, 4, 4};
    const dl::detect::anchor_point_stage_t espdet_stage_16 = {16, 16, 8, 8};
    const dl::detect::anchor_point_stage_t espdet_stage_32 = {32, 32, 16, 16};
    const std::vector<dl::detect::anchor_point_stage_t> espdet_stages = {
        espdet_stage_8,
        espdet_stage_16,
        espdet_stage_32,
    };

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

static dl::image::pix_type_t espdl_image_pix_type(image_t *img)
{
    if (img->pixfmt == PIXFORMAT_RGB565) {
        return dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
    }
    if (img->pixfmt == PIXFORMAT_GRAYSCALE) {
        return dl::image::DL_IMAGE_PIX_TYPE_GRAY;
    }

    mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("ESPDet only supports RGB565 or grayscale images"));
}

static mp_obj_t espdl_espdet_detect(mp_obj_t self_in, mp_obj_t img_in)
{
    espdl_espdet_obj_t *self = (espdl_espdet_obj_t *)MP_OBJ_TO_PTR(self_in);
    espdl_espdet_ensure_active(self);

    if (!mp_obj_is_type(img_in, &py_image_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected image.Image"));
    }

    image_t *img = (image_t *)py_image_cobj(img_in);
    if ((img == nullptr) || (img->pixels == nullptr) || (img->w <= 0) || (img->h <= 0)) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("invalid image"));
    }

    dl::image::img_t dl_img;
    dl_img.data = img->pixels;
    dl_img.width = (uint16_t)img->w;
    dl_img.height = (uint16_t)img->h;
    dl_img.pix_type = espdl_image_pix_type(img);

    self->image_preprocessor->preprocess(dl_img);
    self->model->run();
    self->postprocessor->clear_result();
    self->postprocessor->postprocess();

    std::list<dl::detect::result_t> &results = self->postprocessor->get_result(img->w, img->h);
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

static const mp_rom_map_elem_t espdl_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_espdl)},
    {MP_ROM_QSTR(MP_QSTR_ESPDet), MP_ROM_PTR(&espdl_espdet_type)},
    {MP_ROM_QSTR(MP_QSTR_load_model), MP_ROM_PTR(&espdl_load_model_obj)},
};
static MP_DEFINE_CONST_DICT(espdl_module_globals, espdl_module_globals_table);

extern "C" const mp_obj_module_t espdl_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *) &espdl_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_espdl, espdl_module);

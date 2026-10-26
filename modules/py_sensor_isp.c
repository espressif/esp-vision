/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stddef.h>

#include "py/objlist.h"
#include "py/runtime.h"
#include "camera.h"
#include "camera_isp.h"
#include "py_sensor_isp.h"

static mp_obj_t sensor_stats_uints(const uint32_t *values, size_t count)
{
    mp_obj_t result = mp_obj_new_list(count, NULL);
    mp_obj_list_t *list = MP_OBJ_TO_PTR(result);
    for (size_t i = 0; i < count; i++) {
        list->items[i] = mp_obj_new_int_from_uint(values[i]);
    }
    return result;
}

static mp_obj_t sensor_stats_awb(const esp_vision_isp_awb_stats_t *awb)
{
    mp_obj_t result = mp_obj_new_dict(4);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_counted), mp_obj_new_int_from_uint(awb->counted));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_sum_r), mp_obj_new_int_from_uint(awb->sum_r));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_sum_g), mp_obj_new_int_from_uint(awb->sum_g));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_sum_b), mp_obj_new_int_from_uint(awb->sum_b));
    return result;
}

static mp_obj_t sensor_get_ipa_stats(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_timeout, MP_ARG_INT, {.u_int = 1000}},
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t timeout = args[ARG_timeout].u_int;
    if (timeout < 1 || timeout > 1000) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout must be 1..1000 ms"));
    }
    esp_vision_ipa_stats_t stats;
    // Keep the GIL while the bounded read owns the queue: another Python
    // thread must not reset the camera or stop IPA during this operation.
    esp_err_t ret = esp_vision_camera_get_ipa_stats(&stats, timeout);
    if (ret == ESP_ERR_NOT_SUPPORTED || ret == ESP_ERR_TIMEOUT) {
        return mp_const_none;
    }
    if (ret != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("IPA statistics error: %s"), esp_err_to_name(ret));
    }
    mp_obj_t result = mp_obj_new_dict(8);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_seq), mp_obj_new_int_from_ull(stats.seq));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_flags), mp_obj_new_int_from_uint(stats.flags));
    mp_obj_t ae = mp_const_none;
    if (stats.flags & ESP_VISION_IPA_STATS_AE) {
        ae = mp_obj_new_list(ESP_VISION_ISP_STATS_GRID_X, NULL);
        for (size_t x = 0; x < ESP_VISION_ISP_STATS_GRID_X; x++) {
            ((mp_obj_list_t *)MP_OBJ_TO_PTR(ae))->items[x] =
                sensor_stats_uints(stats.ae[x], ESP_VISION_ISP_STATS_GRID_Y);
        }
    }
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_ae), ae);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_awb),
                      stats.flags & ESP_VISION_IPA_STATS_AWB ? sensor_stats_awb(&stats.awb) : mp_const_none);
    mp_obj_t subwin = mp_const_none;
    if (stats.flags & ESP_VISION_IPA_STATS_AWB_SUBWIN) {
        subwin = mp_obj_new_list(ESP_VISION_ISP_STATS_GRID_X, NULL);
        for (size_t x = 0; x < ESP_VISION_ISP_STATS_GRID_X; x++) {
            mp_obj_t row = mp_obj_new_list(ESP_VISION_ISP_STATS_GRID_Y, NULL);
            ((mp_obj_list_t *)MP_OBJ_TO_PTR(subwin))->items[x] = row;
            for (size_t y = 0; y < ESP_VISION_ISP_STATS_GRID_Y; y++) {
                ((mp_obj_list_t *)MP_OBJ_TO_PTR(row))->items[y] = sensor_stats_awb(&stats.awb_subwin[x][y]);
            }
        }
    }
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_awb_subwin), subwin);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_hist), stats.flags & ESP_VISION_IPA_STATS_HIST ?
                      sensor_stats_uints(stats.hist, ESP_VISION_ISP_STATS_HIST_BINS) : mp_const_none);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_sharpen), stats.flags & ESP_VISION_IPA_STATS_SHARPEN ?
                      mp_obj_new_int(stats.sharpen) : mp_const_none);
    mp_obj_t af = mp_const_none;
    if (stats.flags & ESP_VISION_IPA_STATS_AF) {
        af = mp_obj_new_list(ESP_VISION_ISP_STATS_AF_WINDOWS, NULL);
        for (size_t i = 0; i < ESP_VISION_ISP_STATS_AF_WINDOWS; i++) {
            mp_obj_t window = mp_obj_new_dict(2);
            ((mp_obj_list_t *)MP_OBJ_TO_PTR(af))->items[i] = window;
            mp_obj_dict_store(window, MP_OBJ_NEW_QSTR(MP_QSTR_definition), mp_obj_new_int_from_uint(stats.af[i].definition));
            mp_obj_dict_store(window, MP_OBJ_NEW_QSTR(MP_QSTR_luminance), mp_obj_new_int_from_uint(stats.af[i].luminance));
        }
    }
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_af), af);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_KW(sensor_get_ipa_stats_obj, 0, sensor_get_ipa_stats);

typedef enum {
    ISP_BOOL, ISP_U8, ISP_U16, ISP_I32, ISP_FLOAT, ISP_FLOAT_ARRAY, ISP_U8_ARRAY, ISP_LSC_ARRAY, ISP_SIZE,
} sensor_isp_field_type_t;

typedef struct {
    qstr name;
    size_t offset;
    sensor_isp_field_type_t type;
    size_t count;
} sensor_isp_field_t;

#define FIELD(name, member, type, count) {MP_QSTR_##name, offsetof(esp_vision_isp_config_t, member), type, count}
#define ENABLE FIELD(enable, enable, ISP_BOOL, 1)
static const sensor_isp_field_t s_exposure[] = {FIELD(value, exposure_us, ISP_I32, 1)};
static const sensor_isp_field_t s_pixel_gain[] = {FIELD(value, gain, ISP_FLOAT, 1)};
static const sensor_isp_field_t s_demosaic[] = {ENABLE, FIELD(gradient_ratio, gradient_ratio, ISP_FLOAT, 1)};
static const sensor_isp_field_t s_wb[] = {ENABLE, FIELD(rg, rg, ISP_FLOAT, 1), FIELD(bg, bg, ISP_FLOAT, 1)};
static const sensor_isp_field_t s_lsc[] = {
    ENABLE, FIELD(lsc_gain_size, lsc_gain_size, ISP_SIZE, 1),
    FIELD(gain_r, lsc_gain[0], ISP_LSC_ARRAY, 0), FIELD(gain_gr, lsc_gain[1], ISP_LSC_ARRAY, 0),
    FIELD(gain_gb, lsc_gain[2], ISP_LSC_ARRAY, 0), FIELD(gain_b, lsc_gain[3], ISP_LSC_ARRAY, 0),
};
static const sensor_isp_field_t s_blc[] = {
    ENABLE, FIELD(stretch_enable, stretch_enable, ISP_BOOL, 1),
    FIELD(top_left_offset, offsets[0], ISP_U16, 1), FIELD(top_right_offset, offsets[1], ISP_U16, 1),
    FIELD(bottom_left_offset, offsets[2], ISP_U16, 1), FIELD(bottom_right_offset, offsets[3], ISP_U16, 1),
};
static const sensor_isp_field_t s_bf[] = {
    ENABLE, FIELD(level, level, ISP_U8, 1), FIELD(matrix, matrix, ISP_FLOAT_ARRAY, ESP_VISION_ISP_MATRIX_SIZE),
};
static const sensor_isp_field_t s_sharpen[] = {
    ENABLE, FIELD(h_thresh, h_thresh, ISP_U8, 1), FIELD(l_thresh, l_thresh, ISP_U8, 1),
    FIELD(h_coeff, h_coeff, ISP_FLOAT, 1), FIELD(m_coeff, m_coeff, ISP_FLOAT, 1),
    FIELD(matrix, matrix, ISP_FLOAT_ARRAY, ESP_VISION_ISP_MATRIX_SIZE),
};
static const sensor_isp_field_t s_ccm[] = {ENABLE, FIELD(matrix, matrix, ISP_FLOAT_ARRAY, ESP_VISION_ISP_MATRIX_SIZE)};
static const sensor_isp_field_t s_gamma[] = {
    ENABLE, FIELD(x, x, ISP_U8_ARRAY, ESP_VISION_ISP_GAMMA_POINTS), FIELD(y, y, ISP_U8_ARRAY, ESP_VISION_ISP_GAMMA_POINTS),
};
#undef FIELD
#undef ENABLE

typedef struct {
    qstr name;
    const sensor_isp_field_t *fields;
    size_t count;
} sensor_isp_block_t;

#define BLOCK(name) {MP_QSTR_##name, s_##name, MP_ARRAY_SIZE(s_##name)}
static const sensor_isp_block_t s_blocks[ESP_VISION_ISP_BLOCK_COUNT] = {
    BLOCK(exposure), BLOCK(pixel_gain), BLOCK(demosaic), BLOCK(wb), BLOCK(lsc),
    BLOCK(blc), BLOCK(bf), BLOCK(sharpen), BLOCK(ccm), BLOCK(gamma),
};
#undef BLOCK

static esp_vision_isp_block_t sensor_isp_block(mp_obj_t name_in)
{
    // Look the name up without interning it, so misspelled block names do not
    // permanently consume qstr pool memory.
    size_t len;
    const char *str = mp_obj_str_get_data(name_in, &len);
    qstr name = qstr_find_strn(str, len);
    for (size_t i = 0; name != MP_QSTRnull && i < MP_ARRAY_SIZE(s_blocks); i++) {
        if (s_blocks[i].name == name) {
            return i;
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("unknown ISP block"));
}

static void sensor_isp_raise(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ISP parameters"));
    }
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("ISP error: %s"), esp_err_to_name(err));
}

static esp_err_t sensor_isp_read(esp_vision_isp_block_t block, esp_vision_isp_config_t *config)
{
    if (block != ESP_VISION_ISP_LSC) {
        return esp_vision_camera_get_isp(block, config);
    }
    // Probe the table length first: a read that leaves the destinations NULL
    // only reports lsc_gain_size, which keeps the tables sized to the capture
    // instead of to ESP_VISION_ISP_LSC_MAX_POINTS.
    esp_err_t ret = esp_vision_camera_get_isp(block, config);
    if (ret != ESP_OK) {
        return ret;
    }
    if ((config->lsc_gain_size == 0) || (config->lsc_gain_size > ESP_VISION_ISP_LSC_MAX_POINTS)) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < 4; i++) {
        config->lsc_gain[i] = m_new(float, config->lsc_gain_size);
    }
    return esp_vision_camera_get_isp(block, config);
}

static mp_obj_t sensor_isp_field_get(const sensor_isp_field_t *field, esp_vision_isp_config_t *config)
{
    void *data = (uint8_t *)config + field->offset;
    switch (field->type) {
    case ISP_BOOL:
        return mp_obj_new_bool(*(bool *)data);
    case ISP_U8:
        return mp_obj_new_int(*(uint8_t *)data);
    case ISP_U16:
        return mp_obj_new_int(*(uint16_t *)data);
    case ISP_I32:
        return mp_obj_new_int(*(int32_t *)data);
    case ISP_SIZE:
        return mp_obj_new_int_from_uint(*(size_t *)data);
    case ISP_FLOAT:
        return mp_obj_new_float(*(float *)data);
    default:
        break;
    }
    size_t count = field->type == ISP_LSC_ARRAY ? config->lsc_gain_size : field->count;
    if (field->type == ISP_LSC_ARRAY) {
        data = *(float **)data;
    }
    mp_obj_t result = mp_obj_new_list(count, NULL);
    mp_obj_list_t *list = MP_OBJ_TO_PTR(result);
    for (size_t i = 0; i < count; i++) {
        list->items[i] = field->type == ISP_U8_ARRAY ? mp_obj_new_int(((uint8_t *)data)[i]) :
                         mp_obj_new_float(((float *)data)[i]);
    }
    return result;
}

static float sensor_isp_float(mp_obj_t value)
{
    float result = mp_obj_get_float(value);
    if (!isfinite(result)) {
        mp_raise_ValueError(MP_ERROR_TEXT("ISP values must be finite"));
    }
    return result;
}

static mp_int_t sensor_isp_uint(mp_obj_t value, mp_int_t max)
{
    mp_int_t result = mp_obj_get_int(value);
    if (result < 0 || result > max) {
        mp_raise_ValueError(MP_ERROR_TEXT("ISP integer out of range"));
    }
    return result;
}

static void sensor_isp_field_set(const sensor_isp_field_t *field, esp_vision_isp_config_t *config, mp_obj_t value)
{
    void *data = (uint8_t *)config + field->offset;
    switch (field->type) {
    case ISP_BOOL:
        *(bool *)data = mp_obj_is_true(value);
        return;
    case ISP_U8:
        if (field->name == MP_QSTR_level) {
            mp_int_t level = mp_obj_get_int(value);
            if (level < 2 || level > 20) {
                mp_raise_ValueError(MP_ERROR_TEXT("BF level must be 2..20; use enable=False to disable"));
            }
            *(uint8_t *)data = level;
            return;
        }
        *(uint8_t *)data = sensor_isp_uint(value, UINT8_MAX);
        return;
    case ISP_U16:
        *(uint16_t *)data = sensor_isp_uint(value, UINT16_MAX);
        return;
    case ISP_I32:
        *(int32_t *)data = mp_obj_get_int(value);
        return;
    case ISP_SIZE:
        if ((size_t)sensor_isp_uint(value, ESP_VISION_ISP_LSC_MAX_POINTS) != config->lsc_gain_size) {
            mp_raise_ValueError(MP_ERROR_TEXT("LSC table size must match the capture size"));
        }
        return;
    case ISP_FLOAT:
        *(float *)data = sensor_isp_float(value);
        return;
    default:
        break;
    }
    size_t count;
    mp_obj_t *items;
    mp_obj_get_array(value, &count, &items);
    size_t expected = field->type == ISP_LSC_ARRAY ? config->lsc_gain_size : field->count;
    if (count != expected) {
        mp_raise_ValueError(MP_ERROR_TEXT("ISP array length mismatch"));
    }
    if (field->type == ISP_LSC_ARRAY) {
        data = *(float **)data;
    }
    for (size_t i = 0; i < count; i++) {
        if (field->type == ISP_U8_ARRAY) {
            ((uint8_t *)data)[i] = sensor_isp_uint(items[i], UINT8_MAX);
        } else {
            ((float *)data)[i] = sensor_isp_float(items[i]);
        }
    }
    if (field->name == MP_QSTR_x || field->name == MP_QSTR_y) {
        config->gamma_curve_changed = true;
    }
}

static mp_obj_t sensor_get_isp(mp_obj_t name_in)
{
    esp_vision_isp_block_t block = sensor_isp_block(name_in);
    esp_vision_isp_config_t config = {0};
    esp_err_t ret = sensor_isp_read(block, &config);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        return mp_const_none;
    }
    if (ret != ESP_OK) {
        sensor_isp_raise(ret);
    }
    const sensor_isp_block_t *schema = &s_blocks[block];
    mp_obj_t result = mp_obj_new_dict(schema->count);
    for (size_t i = 0; i < schema->count; i++) {
        mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(schema->fields[i].name), sensor_isp_field_get(&schema->fields[i], &config));
    }
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_1(sensor_get_isp_obj, sensor_get_isp);

static mp_obj_t sensor_set_isp(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    mp_arg_check_num(n_args, kwargs->used, 1, 1, true);
    esp_vision_isp_block_t block = sensor_isp_block(args[0]);
    if (kwargs->used == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("ISP parameters are required"));
    }
    bool ipa_enabled = false;
    esp_err_t ret = esp_vision_camera_get_ipa(&ipa_enabled);
    if (ret == ESP_OK && ipa_enabled) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("disable IPA before setting ISP parameters"));
    }
    esp_vision_isp_config_t config = {0};
    // Scalar controls replace their sole value and do not require readback.
    if (block != ESP_VISION_ISP_EXPOSURE && block != ESP_VISION_ISP_PIXEL_GAIN) {
        ret = sensor_isp_read(block, &config);
        if (ret != ESP_OK) {
            sensor_isp_raise(ret);
        }
    }
    const sensor_isp_block_t *schema = &s_blocks[block];
    size_t used = 0;
    for (size_t i = 0; i < schema->count; i++) {
        const sensor_isp_field_t *field = &schema->fields[i];
        mp_map_elem_t *entry = mp_map_lookup(kwargs, MP_OBJ_NEW_QSTR(field->name), MP_MAP_LOOKUP);
        if (entry != NULL) {
            sensor_isp_field_set(field, &config, entry->value);
            if (field->name != MP_QSTR_enable) {
                config.params_changed = true;
            }
            used++;
        }
    }
    if (used != kwargs->used) {
        mp_raise_TypeError(MP_ERROR_TEXT("unknown parameter for ISP block"));
    }
    ret = esp_vision_camera_set_isp(block, &config);
    if (ret != ESP_OK) {
        sensor_isp_raise(ret);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(sensor_set_isp_obj, 1, sensor_set_isp);

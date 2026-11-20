/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "py/mpconfig.h"
#include "py_uvc.h"

#include "py/runtime.h"
MP_REGISTER_ROOT_POINTER(mp_obj_t py_uvc_mode_callback);

#if MICROPY_HW_ENABLE_USBDEV && (!defined(MICROPY_HW_USB_UVC) || MICROPY_HW_USB_UVC)
#include <string.h>
#include "py/mphal.h"
#include "py_image.h"
#include "uvc.h"

static mp_obj_t py_uvc_send(mp_obj_t img, bool fixed)
{
    bool accepted;
    esp_err_t err = esp_vision_uvc_write(py_image_cobj(img), fixed, &accepted);
    if (err == ESP_ERR_INVALID_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("UVC dimensions or JPEG size invalid"));
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

static mp_obj_t py_uvc_mode_to_obj(uint16_t width, uint16_t height, uint32_t interval_us)
{
    mp_obj_t result = mp_obj_new_dict(0);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_format), MP_OBJ_NEW_QSTR(MP_QSTR_MJPEG));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_width), MP_OBJ_NEW_SMALL_INT(width));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_height), MP_OBJ_NEW_SMALL_INT(height));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_interval_us), MP_OBJ_NEW_SMALL_INT(interval_us));
    return result;
}

static mp_obj_t py_uvc_get_mode(void)
{
    esp_vision_uvc_mode_t current;
    esp_vision_uvc_get_mode(&current);
    return py_uvc_mode_to_obj(current.width, current.height, current.interval_us);
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_uvc_get_mode_obj, py_uvc_get_mode);

#if MICROPY_ENABLE_SCHEDULER && MICROPY_SCHEDULER_STATIC_NODES
static mp_sched_node_t py_uvc_mode_sched_node;
static esp_vision_uvc_mode_t py_uvc_pending_mode;
static volatile uint32_t py_uvc_callback_generation;
static volatile uint32_t py_uvc_pending_generation;
static volatile bool py_uvc_mode_pending;

static void py_uvc_mode_node_remove(void)
{
    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
    mp_sched_node_t *previous = NULL;
    mp_sched_node_t *current = MP_STATE_VM(sched_head);
    while (current != NULL) {
        if (current == &py_uvc_mode_sched_node) {
            if (previous == NULL) {
                MP_STATE_VM(sched_head) = current->next;
            } else {
                previous->next = current->next;
            }
            if (MP_STATE_VM(sched_tail) == current) {
                MP_STATE_VM(sched_tail) = previous;
            }
            break;
        }
        previous = current;
        current = current->next;
    }
    py_uvc_mode_sched_node.callback = NULL;
    py_uvc_mode_sched_node.next = NULL;
    MICROPY_END_ATOMIC_SECTION(atomic_state);
}

static void py_uvc_mode_callback_dispatch(mp_sched_node_t *node)
{
    (void)node;
    esp_vision_uvc_mode_t mode;
    mp_obj_t callback;
    bool deliver;
    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
    deliver = py_uvc_mode_pending &&
              py_uvc_pending_generation == py_uvc_callback_generation;
    if (deliver) {
        mode = py_uvc_pending_mode;
    }
    py_uvc_mode_pending = false;
    callback = MP_STATE_PORT(py_uvc_mode_callback);
    MICROPY_END_ATOMIC_SECTION(atomic_state);

    if (deliver && callback != MP_OBJ_NULL && callback != mp_const_none) {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_obj_t mode_obj = py_uvc_mode_to_obj(mode.width, mode.height, mode.interval_us);
            mp_call_function_1(callback, mode_obj);
            nlr_pop();
        } else {
            mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
        }
    }
}

static void py_uvc_mode_committed(const esp_vision_uvc_mode_t *mode)
{
    if (mode == NULL) {
        return;
    }
    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
    py_uvc_pending_mode = *mode;
    py_uvc_pending_generation = py_uvc_callback_generation;
    py_uvc_mode_pending = true;
    MICROPY_END_ATOMIC_SECTION(atomic_state);

    // TinyUSB's task only copies the mode and queues this static node. Python
    // runs later in the VM task through MicroPython's scheduler.
    mp_sched_schedule_node(&py_uvc_mode_sched_node, py_uvc_mode_callback_dispatch);
    mp_hal_wake_main_task();
}
#endif

static mp_obj_t py_uvc_set_mode_callback(mp_obj_t callback)
{
    if (callback != mp_const_none && !mp_obj_is_callable(callback)) {
        mp_raise_TypeError(MP_ERROR_TEXT("callback must be callable or None"));
    }
#if MICROPY_ENABLE_SCHEDULER && MICROPY_SCHEDULER_STATIC_NODES
    // A previously queued static node may still be waiting in the VM scheduler.
    // Invalidate its payload. It will drain as a no-op, or handle a newer event.
    esp_vision_uvc_set_mode_committed_cb(NULL);
    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
    ++py_uvc_callback_generation;
    py_uvc_mode_pending = false;
    MP_STATE_PORT(py_uvc_mode_callback) = callback;
    MICROPY_END_ATOMIC_SECTION(atomic_state);
    if (callback != mp_const_none) {
        esp_vision_uvc_set_mode_committed_cb(py_uvc_mode_committed);
    }
#else
    if (callback != mp_const_none) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("UVC callbacks require static scheduler nodes"));
    }
    MP_STATE_PORT(py_uvc_mode_callback) = mp_const_none;
#endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_uvc_set_mode_callback_obj, py_uvc_set_mode_callback);

void py_uvc_soft_reset(void)
{
#if MICROPY_ENABLE_SCHEDULER && MICROPY_SCHEDULER_STATIC_NODES
    esp_vision_uvc_set_mode_committed_cb(NULL);
    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
    ++py_uvc_callback_generation;
    py_uvc_mode_pending = false;
    MP_STATE_PORT(py_uvc_mode_callback) = mp_const_none;
    MICROPY_END_ATOMIC_SECTION(atomic_state);
    // This reset hook is called only from the top-level soft-reset path, after
    // the USB producer is disabled and outside scheduler-node dispatch.
    py_uvc_mode_node_remove();
#endif
}

static const mp_rom_map_elem_t py_uvc_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_uvc) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&py_uvc_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_frame), MP_ROM_PTR(&py_uvc_set_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&py_uvc_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind), MP_ROM_PTR(&py_uvc_bind_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_streaming), MP_ROM_PTR(&py_uvc_streaming_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_mode), MP_ROM_PTR(&py_uvc_get_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_mode_callback), MP_ROM_PTR(&py_uvc_set_mode_callback_obj) },
};
static MP_DEFINE_CONST_DICT(py_uvc_globals, py_uvc_globals_table);
const mp_obj_module_t py_uvc_module = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *) &py_uvc_globals };
MP_REGISTER_MODULE(MP_QSTR_uvc, py_uvc_module);
#else
void py_uvc_soft_reset(void)
{
}
#endif

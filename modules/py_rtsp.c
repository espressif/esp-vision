/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_timer.h"
#include "esp_rtsp.h"
#include "media_lib_adapter.h"
#include "media_lib_netif.h"

// Queued H.264 frame: malloc'd by send(), freed by the consumer.
typedef struct {
    uint8_t *buf;
    uint32_t len;
} rtsp_frame_item_t;

// FIFO depth. H.264 frames must not be dropped out of order (a lost P-frame
// corrupts the picture until the next IDR).
#define RTSP_FRAME_QUEUE_DEPTH 16

// Shared between the MicroPython thread (send) and the RTSP task (send_video).
// malloc'd, not on the GC heap: the RTSP task has no GIL.
typedef struct {
    QueueHandle_t queue;
    size_t max_len;
    uint32_t start_ms;     // pts base, set on the first frame
    int width;
    int height;
    int fps;
} py_rtsp_shared_t;

typedef struct _py_rtsp_server_obj {
    mp_obj_base_t base;
    esp_rtsp_handle_t rtsp;
    py_rtsp_shared_t *shared;
    esp_rtsp_video_info_t video_info;  // kept alive for the RTSP task
    esp_rtsp_data_cb_t data_cb;
} py_rtsp_server_obj_t;

// RTSP task (no GIL): pop the next frame in order, hand it over, free it.
static int py_rtsp_send_video(unsigned char *data, unsigned int *len, uint32_t *pts, void *ctx)
{
    py_rtsp_shared_t *sh = (py_rtsp_shared_t *)ctx;

    rtsp_frame_item_t item;
    if (xQueueReceive(sh->queue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
        *len = 0;
        return 0;
    }

    if (item.len <= *len) {
        memcpy(data, item.buf, item.len);
        *len = item.len;
    } else {
        *len = 0;
    }
    free(item.buf);

    // Real elapsed-ms pts (a fixed-fps pts makes players stutter).
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (sh->start_ms == 0) {
        sh->start_ms = now;
    }
    *pts = now - sh->start_ms;
    return 0;
}

// Fills the SDP codec/resolution. Must be non-NULL: RTSP calls it
// unconditionally on DESCRIBE/SETUP.
static int py_rtsp_stream_codec(esp_rtsp_aud_info_t *aud_info, esp_rtsp_video_info_t *vid_info, void *ctx)
{
    py_rtsp_shared_t *sh = (py_rtsp_shared_t *)ctx;
    (void)aud_info;
    if (vid_info != NULL) {
        vid_info->vcodec = RTSP_VCODEC_H264;
        vid_info->width = sh->width;
        vid_info->height = sh->height;
        vid_info->fps = sh->fps;
        vid_info->len = (int)sh->max_len;
    }
    return 0;
}

static int py_rtsp_state(esp_rtsp_state_t event, void *ctx)
{
    (void)event;
    (void)ctx;
    return 0;
}

static mp_obj_t py_rtsp_server_make_new(const mp_obj_type_t *type, size_t n_args,
                                        size_t n_kw, const mp_obj_t *all_args)
{
    enum { ARG_width, ARG_height, ARG_fps, ARG_port, ARG_max_frame_len };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_width,         MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}    },
        { MP_QSTR_height,        MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}    },
        { MP_QSTR_fps,           MP_ARG_INT | MP_ARG_KW_ONLY,  {.u_int = 15}   },
        { MP_QSTR_listen_port,   MP_ARG_INT | MP_ARG_KW_ONLY,  {.u_int = 8554} },
        { MP_QSTR_max_frame_len, MP_ARG_INT | MP_ARG_KW_ONLY,  {.u_int = 0}    },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int width = args[ARG_width].u_int;
    int height = args[ARG_height].u_int;
    int fps = args[ARG_fps].u_int;
    int port = args[ARG_port].u_int;
    int max_frame_len = args[ARG_max_frame_len].u_int;

    if ((width <= 0) || (height <= 0) || (fps <= 0)) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("invalid width/height/fps"));
    }
    if (max_frame_len <= 0) {
        max_frame_len = width * height;  // safe ceiling; encoded frames are far smaller
    }

    // Register media_lib_sal's default adapters once, else server_start fails.
    static bool s_media_lib_ready = false;
    if (!s_media_lib_ready) {
        if (media_lib_add_default_adapter() != ESP_OK) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("rtsp: media_lib init failed"));
        }
        s_media_lib_ready = true;
    }

    py_rtsp_shared_t *sh = malloc(sizeof(py_rtsp_shared_t));
    if (sh == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("rtsp: out of memory"));
    }
    memset(sh, 0, sizeof(*sh));
    sh->queue = xQueueCreate(RTSP_FRAME_QUEUE_DEPTH, sizeof(rtsp_frame_item_t));
    if (sh->queue == NULL) {
        free(sh);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("rtsp: out of memory"));
    }
    sh->max_len = max_frame_len;
    sh->width = width;
    sh->height = height;
    sh->fps = fps;

    py_rtsp_server_obj_t *self = mp_obj_malloc_with_finaliser(py_rtsp_server_obj_t, type);
    self->shared = sh;
    self->video_info = (esp_rtsp_video_info_t) {
        .vcodec = RTSP_VCODEC_H264,
        .width = width,
        .height = height,
        .fps = fps,
        .len = max_frame_len,
    };
    self->data_cb = (esp_rtsp_data_cb_t) {
        .send_video = py_rtsp_send_video,
        .stream_codec = py_rtsp_stream_codec,
    };

    // Bind to our IP (NULL can pick the wrong source interface).
    media_lib_ipv4_info_t ip_info;
    char *local_addr = NULL;
    if (media_lib_netif_get_ipv4_info(MEDIA_LIB_NET_TYPE_STA, &ip_info) == ESP_OK) {
        local_addr = media_lib_ipv4_ntoa(&ip_info.ip);
    }

    esp_rtsp_config_t config = {
        .ctx = sh,
        .video_enable = true,
        .audio_enable = false,
        .local_port = port,
        .local_addr = local_addr,
        .stack_size = 8192,
        .task_prio = 5,
        .mode = RTSP_SERVER,
        .video_info = &self->video_info,
        .data_cb = &self->data_cb,
        .state = py_rtsp_state,
        .trans = RTSP_TRANSPORT_UDP,
    };

    self->rtsp = esp_rtsp_server_start(&config);
    if (self->rtsp == NULL) {
        vQueueDelete(sh->queue);
        free(sh);
        self->shared = NULL;
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("rtsp: server start failed"));
    }

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t py_rtsp_server_send(mp_obj_t self_in, mp_obj_t nal_in)
{
    py_rtsp_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->shared == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("server stopped"));
    }

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(nal_in, &bufinfo, MP_BUFFER_READ);

    py_rtsp_shared_t *sh = self->shared;
    if ((bufinfo.len == 0) || (bufinfo.len > sh->max_len)) {
        return mp_const_none;
    }

    uint8_t *copy = malloc(bufinfo.len);
    if (copy == NULL) {
        return mp_const_none;
    }
    memcpy(copy, bufinfo.buf, bufinfo.len);

    rtsp_frame_item_t item = { .buf = copy, .len = (uint32_t)bufinfo.len };
    // Queue full (no / slow client): drop rather than block the MP thread.
    if (xQueueSend(sh->queue, &item, pdMS_TO_TICKS(50)) != pdTRUE) {
        free(copy);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(py_rtsp_server_send_obj, py_rtsp_server_send);

static mp_obj_t py_rtsp_server_stop(mp_obj_t self_in)
{
    py_rtsp_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->rtsp != NULL) {
        esp_rtsp_server_stop(self->rtsp);  // stops + joins the RTSP task
        self->rtsp = NULL;
    }
    if (self->shared != NULL) {
        rtsp_frame_item_t item;
        while (xQueueReceive(self->shared->queue, &item, 0) == pdTRUE) {
            free(item.buf);
        }
        vQueueDelete(self->shared->queue);
        free(self->shared);
        self->shared = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_rtsp_server_stop_obj, py_rtsp_server_stop);

static const mp_rom_map_elem_t py_rtsp_server_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send),    MP_ROM_PTR(&py_rtsp_server_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop),    MP_ROM_PTR(&py_rtsp_server_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&py_rtsp_server_stop_obj) },
};
static MP_DEFINE_CONST_DICT(py_rtsp_server_locals_dict, py_rtsp_server_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    py_rtsp_server_type,
    MP_QSTR_RTSPServer,
    MP_TYPE_FLAG_NONE,
    make_new, py_rtsp_server_make_new,
    locals_dict, &py_rtsp_server_locals_dict
);

static const mp_rom_map_elem_t rtsp_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),   MP_ROM_QSTR(MP_QSTR_rtsp)        },
    { MP_ROM_QSTR(MP_QSTR_RTSPServer), MP_ROM_PTR(&py_rtsp_server_type) },
};
static MP_DEFINE_CONST_DICT(rtsp_module_globals, rtsp_module_globals_table);

const mp_obj_module_t mp_module_rtsp = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *) &rtsp_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_rtsp, mp_module_rtsp);

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_isp.h"

#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_video_caps.h"
#include "esp_video_device_internal.h"
#include "esp_video_isp_ioctl.h"
#include "hal/isp_ll.h"
#include "linux/videodev2.h"

#include "ev_memory.h"

#if CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
#include "esp_ipa.h"
#include "esp_video_device.h"
#include "esp_video_device_common.h"
#include "esp_video_pipeline_isp.h"
#endif

static const char *TAG = "esp_vision_isp";

// Mirror esp_video's ISP_LSC_GET_GRIDS(): one gain entry per grid of pixel
// pairs plus a boundary entry. esp_video applies the grid height to both axes,
// so match that rather than using ISP_LL_LSC_GRID_WIDTH for the width.
#define ISP_LSC_GRIDS(res) (((res) - 1) / 2 / ISP_LL_LSC_GRID_HEIGHT + 2)

typedef union {
    esp_video_isp_demosaic_t demosaic;
    esp_video_isp_wb_t wb;
    esp_video_isp_lsc_t lsc;
    esp_video_isp_blc_t blc;
    esp_video_isp_bf_t bf;
    esp_video_isp_sharpen_t sharpen;
    esp_video_isp_ccm_t ccm;
    esp_video_isp_gamma_ext_t gamma;
} video_isp_config_t;

static const uint32_t s_control_ids[ESP_VISION_ISP_BLOCK_COUNT] = {
    V4L2_CID_EXPOSURE_ABSOLUTE, V4L2_CID_GAIN,
    V4L2_CID_USER_ESP_ISP_DEMOSAIC, V4L2_CID_USER_ESP_ISP_WB,
    V4L2_CID_USER_ESP_ISP_LSC, V4L2_CID_USER_ESP_ISP_BLC,
    V4L2_CID_USER_ESP_ISP_BF, V4L2_CID_USER_ESP_ISP_SHARPEN,
    V4L2_CID_USER_ESP_ISP_CCM, V4L2_CID_USER_ESP_ISP_GAMMA_EXT,
};

// Disabling white balance while the ISP is running makes esp_video replace the
// red and blue gains with 1.0 and drop the values just written. Keep the last
// gains here so a later enable, and a read while disabled, still see them.
static bool s_wb_gain_held;
static float s_wb_rg;
static float s_wb_bg;

static void isp_wb_hold(const esp_vision_isp_config_t *config)
{
    s_wb_rg = config->rg;
    s_wb_bg = config->bg;
    s_wb_gain_held = true;
}

static void isp_wb_release_hold(void)
{
    s_wb_gain_held = false;
}

static void isp_wb_restore(esp_vision_isp_config_t *config)
{
    if (s_wb_gain_held && !config->enable) {
        config->rg = s_wb_rg;
        config->bg = s_wb_bg;
    }
}

#if ESP_VIDEO_ISP_DEVICE_LSC
// The ISP device stores only pointers to the gain tables (see
// esp_video_isp_lsc_t) and re-reads them on every reconfigure, so a table the
// device has accepted must never be rewritten in place. Keep two banks and
// publish a new one only once the device has taken it. Both the manual writes
// and the save/restore around an IPA switch fill the spare bank, which is safe
// because a save is always followed by its restore before anything else runs.
#define ISP_LSC_BANK_COUNT 2

static isp_lsc_gain_t *s_lsc_banks[ISP_LSC_BANK_COUNT];
static uint8_t s_lsc_installed_bank;
static esp_video_isp_lsc_t s_saved_lsc;
static uint8_t s_saved_bank;
static bool s_lsc_saved;

static esp_err_t isp_lsc_take_spare(uint8_t *bank)
{
    uint8_t spare = s_lsc_installed_bank ^ 1;
    if (s_lsc_banks[spare] == NULL) {
        s_lsc_banks[spare] = esp_vision_memory_calloc(4 * ESP_VISION_ISP_LSC_MAX_POINTS,
                                                      sizeof(isp_lsc_gain_t),
                                                      MALLOC_CAP_8BIT);
        if (s_lsc_banks[spare] == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    *bank = spare;
    return ESP_OK;
}

static void isp_lsc_pointers(esp_video_isp_lsc_t *lsc, const isp_lsc_gain_t *bank)
{
    lsc->gain_r = bank;
    lsc->gain_gr = bank + ESP_VISION_ISP_LSC_MAX_POINTS;
    lsc->gain_gb = bank + 2 * ESP_VISION_ISP_LSC_MAX_POINTS;
    lsc->gain_b = bank + 3 * ESP_VISION_ISP_LSC_MAX_POINTS;
}
#endif

static size_t isp_native_size(esp_vision_isp_block_t block)
{
    switch (block) {
    case ESP_VISION_ISP_DEMOSAIC:
        return sizeof(esp_video_isp_demosaic_t);
    case ESP_VISION_ISP_WB:
        return sizeof(esp_video_isp_wb_t);
    case ESP_VISION_ISP_LSC:
        return sizeof(esp_video_isp_lsc_t);
    case ESP_VISION_ISP_BLC:
        return sizeof(esp_video_isp_blc_t);
    case ESP_VISION_ISP_BF:
        return sizeof(esp_video_isp_bf_t);
    case ESP_VISION_ISP_SHARPEN:
        return sizeof(esp_video_isp_sharpen_t);
    case ESP_VISION_ISP_CCM:
        return sizeof(esp_video_isp_ccm_t);
    case ESP_VISION_ISP_GAMMA:
        return sizeof(esp_video_isp_gamma_ext_t);
    default:
        return 0;
    }
}

static esp_err_t isp_ioctl(int fd, esp_vision_isp_block_t block, void *data, int32_t *value, bool write)
{
    struct v4l2_ext_control control = {.id = s_control_ids[block]};
    struct v4l2_ext_controls controls = {
        .ctrl_class = block == ESP_VISION_ISP_EXPOSURE ? V4L2_CTRL_CLASS_CAMERA : V4L2_CTRL_CLASS_USER,
        .count = 1,
        .controls = &control,
    };
    if (value != NULL) {
        control.value = *value;
    } else {
        control.size = isp_native_size(block);
        control.p_u8 = data;
    }
    if (ioctl(fd, write ? VIDIOC_S_EXT_CTRLS : VIDIOC_G_EXT_CTRLS, &controls) != 0) {
        return ESP_FAIL;
    }
    if (value != NULL) {
        *value = control.value;
    }
    return ESP_OK;
}

static esp_err_t isp_close(int fd, esp_err_t result)
{
    if ((close(fd) != 0) && (result == ESP_OK)) {
        return ESP_FAIL;
    }
    return result;
}

// Scalar and fixed-array conversion is separate from device access.
static void isp_convert(esp_vision_isp_block_t block, video_isp_config_t *v, esp_vision_isp_config_t *c, bool write)
{
#define COPY(native, portable) do { if (write) { (native) = (portable); } else { (portable) = (native); } } while (0)
    switch (block) {
    case ESP_VISION_ISP_DEMOSAIC:
        COPY(v->demosaic.enable, c->enable);
        COPY(v->demosaic.gradient_ratio, c->gradient_ratio);
        break;
    case ESP_VISION_ISP_WB:
        COPY(v->wb.enable, c->enable);
        COPY(v->wb.red_gain, c->rg);
        COPY(v->wb.blue_gain, c->bg);
        break;
    case ESP_VISION_ISP_BLC:
        COPY(v->blc.enable, c->enable);
        COPY(v->blc.stretch_enable, c->stretch_enable);
        COPY(v->blc.top_left_offset, c->offsets[0]);
        COPY(v->blc.top_right_offset, c->offsets[1]);
        COPY(v->blc.bottom_left_offset, c->offsets[2]);
        COPY(v->blc.bottom_right_offset, c->offsets[3]);
        break;
    case ESP_VISION_ISP_BF:
        COPY(v->bf.enable, c->enable);
        COPY(v->bf.level, c->level);
        for (size_t i = 0; i < ESP_VISION_ISP_MATRIX_SIZE; i++) {
            COPY(v->bf.matrix[i / 3][i % 3], c->matrix[i]);
        }
        break;
    case ESP_VISION_ISP_SHARPEN:
        COPY(v->sharpen.enable, c->enable);
        COPY(v->sharpen.h_thresh, c->h_thresh);
        COPY(v->sharpen.l_thresh, c->l_thresh);
        COPY(v->sharpen.h_coeff, c->h_coeff);
        COPY(v->sharpen.m_coeff, c->m_coeff);
        for (size_t i = 0; i < ESP_VISION_ISP_MATRIX_SIZE; i++) {
            COPY(v->sharpen.matrix[i / 3][i % 3], c->matrix[i]);
        }
        break;
    case ESP_VISION_ISP_CCM:
        COPY(v->ccm.enable, c->enable);
        for (size_t i = 0; i < ESP_VISION_ISP_MATRIX_SIZE; i++) {
            COPY(v->ccm.matrix[i / 3][i % 3], c->matrix[i]);
        }
        break;
    case ESP_VISION_ISP_GAMMA:
        COPY(v->gamma.enable, c->enable);
        for (size_t i = 0; i < ESP_VISION_ISP_GAMMA_POINTS; i++) {
            COPY(v->gamma.red_points[i].x, c->x[i]);
            COPY(v->gamma.red_points[i].y, c->y[i]);
            if (write) {
                v->gamma.green_points[i] = v->gamma.red_points[i];
                v->gamma.blue_points[i] = v->gamma.red_points[i];
            }
        }
        v->gamma.flags = c->gamma_curve_changed ?
                         ESP_VIDEO_ISP_GAMMA_EXT_FLAG_RED | ESP_VIDEO_ISP_GAMMA_EXT_FLAG_GREEN | ESP_VIDEO_ISP_GAMMA_EXT_FLAG_BLUE : 0;
        break;
    default:
        break;
    }
#undef COPY
}

static esp_err_t isp_gain_value(int fd, int32_t index, int64_t *value)
{
    struct v4l2_querymenu menu = {.id = V4L2_CID_GAIN, .index = index};
    if (ioctl(fd, VIDIOC_QUERYMENU, &menu) != 0) {
        return ESP_FAIL;
    }
    *value = menu.value;
    return ESP_OK;
}

static esp_err_t isp_camera_control(int fd, esp_vision_isp_block_t block,
                                    esp_vision_isp_config_t *c, struct v4l2_query_ext_ctrl *query, bool write)
{
    int32_t value = 0;
    if (write && block == ESP_VISION_ISP_EXPOSURE) {
        // The control counts 100us units. Round to the nearest unit so that a
        // request is quantized rather than silently shortened.
        value = (c->exposure_us + 50) / 100;
        if (value < query->minimum || value > query->maximum || query->step == 0 ||
                ((value - query->minimum) % query->step) != 0) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (write) {
        if (query->step == 0) {
            return ESP_FAIL;
        }
        double target = (double)c->gain * 1000;
        double distance = INFINITY;
        int64_t first = 0;
        int64_t last = 0;
        bool probed = false;
        // Gain menu entries increase monotonically, so stop as soon as they
        // start moving away from the target instead of walking the whole menu.
        for (int64_t i = query->minimum; i <= query->maximum; i += query->step) {
            int64_t gain;
            if (isp_gain_value(fd, i, &gain) != ESP_OK) {
                return ESP_FAIL;
            }
            if (!probed) {
                first = gain;
                probed = true;
            }
            last = gain;
            double delta = fabs(gain - target);
            if (delta >= distance) {
                break;
            }
            distance = delta;
            value = i;
        }
        if (!probed || target < first || target > last) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    esp_err_t ret = isp_ioctl(fd, block, NULL, &value, write);
    if (ret != ESP_OK) {
        // Sensor drivers may implement these controls write-only; SC2336 has no
        // getter for absolute exposure, for instance. Report that as
        // unsupported so a read can degrade instead of raising.
        return write ? ret : ESP_ERR_NOT_SUPPORTED;
    }
    if (!write) {
        if (block == ESP_VISION_ISP_EXPOSURE) {
            if (value < 0 || value > INT32_MAX / 100) {
                return ESP_ERR_INVALID_SIZE;
            }
            c->exposure_us = value * 100;
        } else {
            int64_t gain;
            ret = isp_gain_value(fd, value, &gain);
            if (ret == ESP_OK) {
                c->gain = gain / 1000.0f;
            }
        }
    }
    return ret;
}

static esp_err_t isp_lsc_control(int fd, esp_vision_isp_config_t *c, bool write)
{
#if ESP_VIDEO_ISP_DEVICE_LSC
    struct v4l2_format format = {.type = V4L2_BUF_TYPE_META_CAPTURE};
    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0 || format.fmt.pix.width == 0 || format.fmt.pix.height == 0) {
        return ESP_FAIL;
    }
    size_t count = ISP_LSC_GRIDS(format.fmt.pix.width) * ISP_LSC_GRIDS(format.fmt.pix.height);
    if (count > ESP_VISION_ISP_LSC_MAX_POINTS) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_video_isp_lsc_t lsc = {.enable = c->enable, .lsc_gain_size = count};
    if (write) {
        if (c->enable) {
            if (c->lsc_gain_size != count) {
                return ESP_ERR_INVALID_ARG;
            }
            uint8_t bank;
            esp_err_t ret = isp_lsc_take_spare(&bank);
            if (ret != ESP_OK) {
                return ret;
            }
            for (size_t channel = 0; channel < 4; channel++) {
                isp_lsc_gain_t *dst = s_lsc_banks[bank] + channel * ESP_VISION_ISP_LSC_MAX_POINTS;
                for (size_t i = 0; i < count; i++) {
                    // 2.8 fixed point. Round to the nearest 1/256 instead of
                    // truncating, and clamp so the 2-bit integer field cannot wrap.
                    long gain = lroundf(c->lsc_gain[channel][i] * 256.0f);
                    if (gain < 0) {
                        gain = 0;
                    } else if (gain > 1023) {
                        gain = 1023;
                    }
                    dst[i] = (isp_lsc_gain_t) {
                        .integer = (uint32_t)gain / 256, .decimal = (uint32_t)gain % 256,
                    };
                }
            }
            isp_lsc_pointers(&lsc, s_lsc_banks[bank]);
            ret = isp_ioctl(fd, ESP_VISION_ISP_LSC, &lsc, NULL, true);
            if (ret == ESP_OK) {
                s_lsc_installed_bank = bank;
            }
            return ret;
        }
        return isp_ioctl(fd, ESP_VISION_ISP_LSC, &lsc, NULL, true);
    }
    esp_err_t ret = isp_ioctl(fd, ESP_VISION_ISP_LSC, &lsc, NULL, false);
    if (ret != ESP_OK) {
        return ret;
    }
    c->enable = lsc.enable;
    c->lsc_gain_size = count;
    const isp_lsc_gain_t *gains[4] = {lsc.gain_r, lsc.gain_gr, lsc.gain_gb, lsc.gain_b};
    for (size_t channel = 0; channel < 4; channel++) {
        // A NULL destination means the caller is only after lsc_gain_size.
        if (c->lsc_gain[channel] == NULL) {
            continue;
        }
        if (gains[channel] && lsc.lsc_gain_size != count) {
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i < count; i++) {
            c->lsc_gain[channel][i] = gains[channel] ? gains[channel][i].integer + gains[channel][i].decimal / 256.0f : 1.0f;
        }
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t isp_control(int camera_fd, const char *isp_path, esp_vision_isp_block_t block,
                             esp_vision_isp_config_t *c, bool write)
{
    if (block >= ESP_VISION_ISP_BLOCK_COUNT || c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (isp_path == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (write) {
        esp_err_t ret = esp_vision_isp_validate(block, c);
        if (ret != ESP_OK) {
            return ret;
        }
        if (block == ESP_VISION_ISP_DEMOSAIC && (c->enable || c->params_changed) &&
                c->gradient_ratio > (1 << ISP_DEMOSAIC_GRAD_RATIO_INT_BITS) -
                1.0f / (1 << ISP_DEMOSAIC_GRAD_RATIO_DEC_BITS)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    bool camera = block == ESP_VISION_ISP_EXPOSURE || block == ESP_VISION_ISP_PIXEL_GAIN;
    int fd = camera ? camera_fd : open(isp_path, O_RDWR);
    if (fd < 0) {
        return ESP_FAIL;
    }
    struct v4l2_query_ext_ctrl query = {.id = s_control_ids[block]};
    esp_err_t ret;
    if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &query) != 0 || (query.flags & V4L2_CTRL_FLAG_DISABLED)) {
        ret = ESP_ERR_NOT_SUPPORTED;
    } else if (camera) {
        ret = isp_camera_control(fd, block, c, &query, write);
    } else if (block == ESP_VISION_ISP_LSC) {
        ret = isp_lsc_control(fd, c, write);
    } else {
        video_isp_config_t native = {0};
        if (write) {
            isp_convert(block, &native, c, true);
        }
        ret = isp_ioctl(fd, block, &native, NULL, write);
        if (ret == ESP_OK && block == ESP_VISION_ISP_WB) {
            if (write) {
                if (c->enable) {
                    isp_wb_release_hold();
                } else {
                    isp_wb_hold(c);
                }
            } else {
                isp_convert(block, &native, c, false);
                isp_wb_restore(c);
            }
        } else if (ret == ESP_OK && !write) {
            isp_convert(block, &native, c, false);
        }
    }
    return camera ? ret : isp_close(fd, ret);
}

esp_err_t esp_vision_video_isp_get(int camera_fd, const char *isp_path, esp_vision_isp_block_t block,
                                   esp_vision_isp_config_t *config)
{
    return isp_control(camera_fd, isp_path, block, config, false);
}

esp_err_t esp_vision_video_isp_set(int camera_fd, const char *isp_path, esp_vision_isp_block_t block,
                                   const esp_vision_isp_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_vision_isp_config_t copy = *config;
    return isp_control(camera_fd, isp_path, block, &copy, true);
}

esp_err_t esp_vision_video_isp_save_lsc(const char *isp_path)
{
#if ESP_VIDEO_ISP_DEVICE_LSC
    s_lsc_saved = false;
    int fd = open(isp_path, O_RDWR);
    if (fd < 0) {
        return ESP_FAIL;
    }
    esp_video_isp_lsc_t lsc = {0};
    esp_err_t ret = isp_ioctl(fd, ESP_VISION_ISP_LSC, &lsc, NULL, false);
    if (ret == ESP_OK && lsc.lsc_gain_size > 0) {
        const isp_lsc_gain_t *gains[4] = {lsc.gain_r, lsc.gain_gr, lsc.gain_gb, lsc.gain_b};
        if (lsc.lsc_gain_size > ESP_VISION_ISP_LSC_MAX_POINTS || !gains[0] || !gains[1] || !gains[2] || !gains[3]) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            // Copy into the spare bank so the tables the device is still using
            // stay intact until restore publishes this copy.
            uint8_t bank;
            ret = isp_lsc_take_spare(&bank);
            if (ret == ESP_OK) {
                for (size_t i = 0; i < 4; i++) {
                    memcpy(s_lsc_banks[bank] + i * ESP_VISION_ISP_LSC_MAX_POINTS, gains[i],
                           lsc.lsc_gain_size * sizeof(isp_lsc_gain_t));
                }
                s_saved_lsc = lsc;
                isp_lsc_pointers(&s_saved_lsc, s_lsc_banks[bank]);
                s_saved_bank = bank;
                s_lsc_saved = true;
            }
        }
    }
    return isp_close(fd, ret);
#else
    return ESP_OK;
#endif
}

esp_err_t esp_vision_video_isp_restore_lsc(const char *isp_path)
{
#if ESP_VIDEO_ISP_DEVICE_LSC
    if (s_lsc_saved) {
        int fd = open(isp_path, O_RDWR);
        if (fd < 0) {
            return ESP_FAIL;
        }
        // Install owned tables before capture resumes, even if LSC was disabled.
        esp_video_isp_lsc_t lsc = s_saved_lsc;
        lsc.enable = true;
        esp_err_t ret = isp_ioctl(fd, ESP_VISION_ISP_LSC, &lsc, NULL, true);
        if (ret == ESP_OK) {
            s_lsc_installed_bank = s_saved_bank;
            if (!s_saved_lsc.enable) {
                ret = isp_ioctl(fd, ESP_VISION_ISP_LSC, &s_saved_lsc, NULL, true);
            }
        }
        return isp_close(fd, ret);
    }
#endif
    return ESP_OK;
}

void esp_vision_video_isp_release(bool free_lsc_tables)
{
    isp_wb_release_hold();
#if ESP_VIDEO_ISP_DEVICE_LSC
    s_lsc_saved = false;
    memset(&s_saved_lsc, 0, sizeof(s_saved_lsc));
    if (!free_lsc_tables) {
        return;
    }
    for (size_t i = 0; i < ISP_LSC_BANK_COUNT; i++) {
        heap_caps_free(s_lsc_banks[i]);
        s_lsc_banks[i] = NULL;
    }
    s_lsc_installed_bank = 0;
    s_saved_bank = 0;
#else
    (void)free_lsc_tables;
#endif
}

bool esp_vision_video_isp_controller_active(void)
{
#if CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
    return esp_video_isp_pipeline_is_initialized();
#else
    return false;
#endif
}

#if CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
esp_err_t esp_vision_video_isp_lookup_ipa(const char *dev_path, const char *meta_path, const void **ipa_config)
{
    if (ipa_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ipa_config = NULL;
    if ((dev_path == NULL) || (meta_path == NULL) ||
            (strcmp(dev_path, ESP_VIDEO_MIPI_CSI_DEVICE_NAME) != 0)) {
        return ESP_OK;
    }

    esp_video_cam_t cam = {0};
    esp_err_t ret = esp_video_device_common_get_video_cam(CSI_NAME, &cam);
    if (ret != ESP_OK) {
        return ret;
    }
    if ((cam.sensor != NULL) && (cam.sensor->cur_format != NULL) &&
            (cam.sensor->cur_format->isp_info != NULL)) {
        *ipa_config = esp_ipa_pipeline_get_config(cam.sensor->name);
    }
    return ESP_OK;
}

esp_err_t esp_vision_video_isp_get_controller(bool ready, const void *ipa_config, bool *enabled)
{
    if (enabled == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *enabled = false;
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ipa_config == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *enabled = esp_video_isp_pipeline_is_initialized();
    return ESP_OK;
}

esp_err_t esp_vision_video_isp_set_controller(bool enable, int *fd, bool *streaming, bool *initialized,
                                              const char *isp_path, const char *cam_path, const void *ipa_config,
                                              const esp_vision_camera_restart_ops_t *ops)
{
    esp_err_t ret;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if ((fd == NULL) || (streaming == NULL) || (initialized == NULL) || (ops == NULL) ||
            (ops->release_buffers == NULL) || (ops->open_device == NULL) ||
            (ops->set_input_format == NULL) || (ops->update_active_window == NULL) ||
            (ops->init_buffers == NULL) || (ops->start_stream == NULL) || (ops->cleanup == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ioctl(*fd, VIDIOC_STREAMOFF, &type) != 0) {
        ESP_LOGE(TAG, "failed to stop camera stream for IPA change");
        return ESP_FAIL;
    }
    *streaming = false;
    *initialized = false;
    // The pipeline reconfigures the ISP, so gains remembered for manual control
    // no longer match the hardware.
    isp_wb_release_hold();
    if (!enable) {
        ret = esp_vision_video_isp_save_lsc(isp_path);
        if (ret != ESP_OK) {
            goto fail;
        }
    }
    ops->release_buffers();
    if (close(*fd) != 0) {
        ret = ESP_FAIL;
        goto fail;
    }
    *fd = -1;

    if (enable) {
        esp_video_isp_config_t config = {
            .isp_dev = isp_path,
            .cam_dev = cam_path,
            .ipa_config = (const esp_ipa_config_t *)ipa_config,
        };
        ret = esp_video_isp_pipeline_init(&config);
    } else {
        ret = esp_video_isp_pipeline_deinit();
    }
    if (ret != ESP_OK) {
        goto fail;
    }

    if (!enable) {
        ret = esp_vision_video_isp_restore_lsc(isp_path);
        if (ret != ESP_OK) {
            goto fail;
        }
    }

    ret = ops->open_device();
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = ops->set_input_format();
    if (ret != ESP_OK) {
        goto fail;
    }
    ops->update_active_window();
    ret = ops->init_buffers();
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = ops->start_stream();
    if (ret != ESP_OK) {
        goto fail;
    }
    *initialized = true;
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "failed to change IPA state: %s", esp_err_to_name(ret));
    ops->cleanup();
    return ret;
}
#endif

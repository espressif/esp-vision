/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_isp.h"

#include <math.h>

__attribute__((weak)) esp_err_t esp_vision_camera_set_ipa(bool enable)
{
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t esp_vision_camera_get_ipa(bool *enabled)
{
    if (enabled == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *enabled = false;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t esp_vision_camera_get_isp(esp_vision_isp_block_t block, esp_vision_isp_config_t *config)
{
    (void)block;
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_vision_camera_get_ipa_stats(esp_vision_ipa_stats_t *stats, uint32_t timeout_ms)
{
    if (stats == NULL || timeout_ms == 0 || timeout_ms > 1000) {
        return ESP_ERR_INVALID_ARG;
    }
    bool enabled;
    esp_err_t ret = esp_vision_camera_get_ipa(&enabled);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vision_video_isp_get_stats(stats, timeout_ms);
}

__attribute__((weak)) esp_err_t esp_vision_camera_set_isp(esp_vision_isp_block_t block, const esp_vision_isp_config_t *config)
{
    (void)block;
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

static bool in_range(float value, float min, float max)
{
    return isfinite(value) && value >= min && value <= max;
}

esp_err_t esp_vision_isp_validate(esp_vision_isp_block_t block, const esp_vision_isp_config_t *c)
{
    if (c == NULL || block >= ESP_VISION_ISP_BLOCK_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (block == ESP_VISION_ISP_EXPOSURE) {
        // Only reject values no sensor could use: the device control counts
        // 100us units and rounds to the nearest one, so leave headroom for that
        // rounding. The usable range is the sensor's, and the backend checks it
        // against the range the driver reports.
        return (c->exposure_us > 0) && (c->exposure_us <= INT32_MAX - 50) ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (block == ESP_VISION_ISP_PIXEL_GAIN) {
        return isfinite(c->gain) && c->gain > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (!c->enable && !c->params_changed) {
        return ESP_OK;
    }
    switch (block) {
    case ESP_VISION_ISP_DEMOSAIC:
        return isfinite(c->gradient_ratio) && c->gradient_ratio >= 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
    case ESP_VISION_ISP_WB:
        // The same 2.8 fixed-point range the WBG gain registers can express;
        // the CCM-based white balance fallback covers this range as well.
        return in_range(c->rg, 0, 1023.0f / 256) &&
               in_range(c->bg, 0, 1023.0f / 256) ? ESP_OK : ESP_ERR_INVALID_ARG;
    case ESP_VISION_ISP_BLC:
        for (size_t i = 0; i < 4; i++) {
            if (c->offsets[i] > 255) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        break;
    case ESP_VISION_ISP_BF:
    case ESP_VISION_ISP_SHARPEN:
        if (block == ESP_VISION_ISP_BF) {
            if (c->level < 2 || c->level > 20) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (c->l_thresh > c->h_thresh || !in_range(c->h_coeff, 0, 255.0f / 32) ||
                   !in_range(c->m_coeff, 0, 255.0f / 32)) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t i = 0; i < ESP_VISION_ISP_MATRIX_SIZE; i++) {
            if (!in_range(c->matrix[i], 0, block == ESP_VISION_ISP_BF ? 15 : 31) ||
                    truncf(c->matrix[i]) != c->matrix[i]) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        break;
    case ESP_VISION_ISP_CCM:
        for (size_t i = 0; i < ESP_VISION_ISP_MATRIX_SIZE; i++) {
            if (!isfinite(c->matrix[i]) || c->matrix[i] <= -4 || c->matrix[i] >= 4) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        break;
    case ESP_VISION_ISP_GAMMA: {
        uint32_t previous = 0;
        for (size_t i = 0; i < ESP_VISION_ISP_GAMMA_POINTS; i++) {
            uint32_t x = c->x[i];
            if (i == ESP_VISION_ISP_GAMMA_POINTS - 1) {
                if (x != 255) {
                    return ESP_ERR_INVALID_ARG;
                }
                x = 256;
            }
            uint32_t delta = x - previous;
            if (x <= previous || (delta & (delta - 1))) {
                return ESP_ERR_INVALID_ARG;
            }
            previous = x;
        }
        break;
    }
    case ESP_VISION_ISP_LSC:
        if (c->lsc_gain_size == 0 || c->lsc_gain_size > ESP_VISION_ISP_LSC_MAX_POINTS) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t channel = 0; channel < 4; channel++) {
            if (c->lsc_gain[channel] == NULL) {
                return ESP_ERR_INVALID_ARG;
            }
            for (size_t i = 0; i < c->lsc_gain_size; i++) {
                if (!in_range(c->lsc_gain[channel][i], 0, 1023.0f / 256)) {
                    return ESP_ERR_INVALID_ARG;
                }
            }
        }
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

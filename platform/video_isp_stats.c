/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_isp.h"
#include "sdkconfig.h"
#include <string.h>

#if CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
#include "esp_video_caps.h"
#include "esp_video_isp_pipeline.h"
#endif

esp_err_t esp_vision_video_isp_get_stats(esp_vision_ipa_stats_t *stats, uint32_t timeout_ms)
{
    if (stats == NULL || timeout_ms == 0 || timeout_ms > 1000) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
    _Static_assert(ISP_AE_BLOCK_X_NUM == ESP_VISION_ISP_STATS_GRID_X &&
                   ISP_AE_BLOCK_Y_NUM == ESP_VISION_ISP_STATS_GRID_Y, "unexpected AE grid");
    _Static_assert(ISP_HIST_SEGMENT_NUMS == ESP_VISION_ISP_STATS_HIST_BINS, "unexpected histogram size");
    _Static_assert(ISP_AF_WINDOW_NUM == ESP_VISION_ISP_STATS_AF_WINDOWS, "unexpected AF window count");

    // A request-scoped queue avoids stale samples and leaves no background
    // copying cost after the read. Do not stop a queue owned by another user
    // if start fails. Always release our queue before returning to Python.
    esp_err_t ret = esp_video_isp_pipeline_start_dump_stats(1);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_video_isp_stats_t raw = {0};
    ret = esp_video_isp_pipeline_dump_stats(&raw, timeout_ms);
    esp_err_t stop_ret = esp_video_isp_pipeline_stop_dump_stats();
    if (stop_ret != ESP_OK) {
        return stop_ret;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    memset(stats, 0, sizeof(*stats));
    stats->seq = raw.seq;
    // ESP Video swaps the AE/AWB flag bits relative to IPA. Translate them
    // explicitly instead of exposing the backend's bit assignments.
    if (raw.flags & ESP_VIDEO_ISP_STATS_FLAG_AE) {
        stats->flags |= ESP_VISION_IPA_STATS_AE;
        for (size_t x = 0; x < ESP_VISION_ISP_STATS_GRID_X; x++) {
            for (size_t y = 0; y < ESP_VISION_ISP_STATS_GRID_Y; y++) {
                stats->ae[x][y] = raw.ae.ae_result.luminance[x][y];
            }
        }
    }
    if (raw.flags & ESP_VIDEO_ISP_STATS_FLAG_AWB) {
        stats->flags |= ESP_VISION_IPA_STATS_AWB;
        stats->awb.counted = raw.awb.awb_result.white_patch_num;
        stats->awb.sum_r = raw.awb.awb_result.sum_r;
        stats->awb.sum_g = raw.awb.awb_result.sum_g;
        stats->awb.sum_b = raw.awb.awb_result.sum_b;
#if ESP_VIDEO_ISP_DEVICE_AWB_SUBWIN
        _Static_assert(ISP_AWB_WINDOW_X_NUM == ESP_VISION_ISP_STATS_GRID_X &&
                       ISP_AWB_WINDOW_Y_NUM == ESP_VISION_ISP_STATS_GRID_Y, "unexpected AWB grid");
        if (raw.flags & ESP_VIDEO_ISP_STATS_FLAG_AWB_SUBWIN) {
            stats->flags |= ESP_VISION_IPA_STATS_AWB_SUBWIN;
            for (size_t x = 0; x < ESP_VISION_ISP_STATS_GRID_X; x++) {
                for (size_t y = 0; y < ESP_VISION_ISP_STATS_GRID_Y; y++) {
                    esp_vision_isp_awb_stats_t *cell = &stats->awb_subwin[x][y];
                    cell->counted = raw.awb.awb_result.subwin_result.white_patch_num[x][y];
                    cell->sum_r = raw.awb.awb_result.subwin_result.sum_r[x][y];
                    cell->sum_g = raw.awb.awb_result.subwin_result.sum_g[x][y];
                    cell->sum_b = raw.awb.awb_result.subwin_result.sum_b[x][y];
                }
            }
        }
#endif
    }
    if (raw.flags & ESP_VIDEO_ISP_STATS_FLAG_HIST) {
        stats->flags |= ESP_VISION_IPA_STATS_HIST;
        for (size_t i = 0; i < ESP_VISION_ISP_STATS_HIST_BINS; i++) {
            stats->hist[i] = raw.hist.hist_result.hist_value[i];
        }
    }
    if (raw.flags & ESP_VIDEO_ISP_STATS_FLAG_SHARPEN) {
        stats->flags |= ESP_VISION_IPA_STATS_SHARPEN;
        stats->sharpen = raw.sharpen.high_freq_pixel_max;
    }
    if (raw.flags & ESP_VIDEO_ISP_STATS_FLAG_AF) {
        stats->flags |= ESP_VISION_IPA_STATS_AF;
        for (size_t i = 0; i < ESP_VISION_ISP_STATS_AF_WINDOWS; i++) {
            stats->af[i].definition = raw.af.af_result.definition[i];
            stats->af[i].luminance = raw.af.af_result.luminance[i];
        }
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

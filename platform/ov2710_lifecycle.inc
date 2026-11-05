/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define OV2710_LIFECYCLE_WAIT_TICKS ((pdMS_TO_TICKS(2000) > 0) ? pdMS_TO_TICKS(2000) : 1)

static void ov2710_stats_barrier(void *context, uint32_t unused)
{
    (void)unused;
    struct ov2710_cam *cam = context;
    xSemaphoreGive(cam->wb_barrier);
}

static esp_err_t ov2710_stats_wait(struct ov2710_cam *cam)
{
    if (!cam->wb_barrier_pending) {
        if (xTimerPendFunctionCall(ov2710_stats_barrier, cam, 0, OV2710_LIFECYCLE_WAIT_TICKS) != pdPASS) {
            return ESP_ERR_TIMEOUT;
        }
        cam->wb_barrier_pending = true;
    }
    if (xSemaphoreTake(cam->wb_barrier, OV2710_LIFECYCLE_WAIT_TICKS) != pdTRUE) {
        // The queued acknowledgement owns cam until a later successful wait.
        return ESP_ERR_TIMEOUT;
    }
    cam->wb_barrier_pending = false;
    return ESP_OK;
}

esp_err_t esp_vision_ov2710_prepare_delete(esp_cam_sensor_device_t *dev)
{
    if ((dev == NULL) || (dev->priv == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTaskGetCurrentTaskHandle() == xTimerGetTimerDaemonTaskHandle()) {
        return ESP_ERR_INVALID_STATE;
    }
    struct ov2710_cam *cam = dev->priv;
    cam->wb_delete_started = true;
    if (cam->wb_barrier_pending) {
        esp_err_t ret = ov2710_stats_wait(cam);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (cam->wb_delete_complete) {
        return ESP_OK;
    }
    if (cam->wb_timer_handle != NULL) {
        if (xTimerStop(cam->wb_timer_handle, OV2710_LIFECYCLE_WAIT_TICKS) != pdPASS) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t ret = ov2710_stats_wait(cam);
        if (ret != ESP_OK) {
            return ret;
        }
        if (xTimerDelete(cam->wb_timer_handle, OV2710_LIFECYCLE_WAIT_TICKS) != pdPASS) {
            return ESP_ERR_TIMEOUT;
        }
        // Delete is queued, so the handle may already be freed. Never reuse it,
        // including when enqueueing/waiting for its acknowledgement fails.
        cam->wb_timer_handle = NULL;
    }
    esp_err_t ret = ov2710_stats_wait(cam);
    if (ret == ESP_OK) {
        cam->wb_delete_complete = true;
    }
    return ret;
}

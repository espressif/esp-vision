/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_camera.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_vendor.h"

#include "dev_custom.h"

#include "boardconfig.h"

static esp_err_t (*s_st7789_draw_bitmap)(esp_lcd_panel_t *panel,
                                         int x_start,
                                         int y_start,
                                         int x_end,
                                         int y_end,
                                         const void *color_data);

static esp_err_t esp_vision_s3_eye_draw_bitmap(esp_lcd_panel_t *panel,
                                               int x_start,
                                               int y_start,
                                               int x_end,
                                               int y_end,
                                               const void *color_data)
{
    uint16_t *pixels = (uint16_t *)color_data;
    size_t count = (size_t)(x_end - x_start) * (size_t)(y_end - y_start);

    for (size_t i = 0; i < count; i++) {
        pixels[i] = (uint16_t)((pixels[i] >> 8) | (pixels[i] << 8));
    }
    return s_st7789_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_data);
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_config,
                                    esp_lcd_panel_handle_t *panel_handle)
{
    esp_err_t ret = esp_lcd_new_panel_st7789(io, panel_config, panel_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    s_st7789_draw_bitmap = (*panel_handle)->draw_bitmap;
    (*panel_handle)->draw_bitmap = esp_vision_s3_eye_draw_bitmap;
    return ESP_OK;
}

static int esp_vision_s3_eye_camera_init(void *config,
                                         int config_size,
                                         void **device_handle)
{
    (void)config;
    (void)config_size;

    const camera_config_t camera_config = {
        .pin_pwdn = ESP_VISION_CAMERA_SENSOR_PWDN_PIN,
        .pin_reset = ESP_VISION_CAMERA_SENSOR_RESET_PIN,
        .pin_xclk = ESP_VISION_CAMERA_XCLK_PIN,
        .pin_sccb_sda = ESP_VISION_CAMERA_SCCB_I2C_SDA_PIN,
        .pin_sccb_scl = ESP_VISION_CAMERA_SCCB_I2C_SCL_PIN,
        .pin_d7 = ESP_VISION_CAMERA_DVP_D7_PIN,
        .pin_d6 = ESP_VISION_CAMERA_DVP_D6_PIN,
        .pin_d5 = ESP_VISION_CAMERA_DVP_D5_PIN,
        .pin_d4 = ESP_VISION_CAMERA_DVP_D4_PIN,
        .pin_d3 = ESP_VISION_CAMERA_DVP_D3_PIN,
        .pin_d2 = ESP_VISION_CAMERA_DVP_D2_PIN,
        .pin_d1 = ESP_VISION_CAMERA_DVP_D1_PIN,
        .pin_d0 = ESP_VISION_CAMERA_DVP_D0_PIN,
        .pin_vsync = ESP_VISION_CAMERA_DVP_VSYNC_PIN,
        .pin_href = ESP_VISION_CAMERA_DVP_HSYNC_PIN,
        .pin_pclk = ESP_VISION_CAMERA_DVP_PCLK_PIN,
        .xclk_freq_hz = ESP_VISION_CAMERA_XCLK_FREQ,
        .ledc_timer = (ledc_timer_t)ESP_VISION_CAMERA_XCLK_LEDC_TIMER,
        .ledc_channel = (ledc_channel_t)ESP_VISION_CAMERA_XCLK_LEDC_CHANNEL,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = ESP_VISION_CAMERA_BUFFER_COUNT,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = ESP_VISION_CAMERA_SCCB_I2C_PORT,
    };

    esp_err_t ret = esp_camera_init(&camera_config);
    if (ret != ESP_OK) {
        return ret;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        (void)esp_camera_deinit();
        return ESP_FAIL;
    }

    *device_handle = sensor;
    return ESP_OK;
}

static int esp_vision_s3_eye_camera_deinit(void *device_handle)
{
    (void)device_handle;
    return esp_camera_deinit();
}

CUSTOM_DEVICE_IMPLEMENT(camera,
                        esp_vision_s3_eye_camera_init,
                        esp_vision_s3_eye_camera_deinit);

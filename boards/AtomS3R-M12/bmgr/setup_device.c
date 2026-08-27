/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "driver/gpio.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dev_custom.h"

#include "boardconfig.h"

static esp_err_t esp_vision_atom_s3r_m12_camera_set_power(bool enable)
{
#if ESP_VISION_CAMERA_POWER_PIN >= 0
    const gpio_config_t io_config = {
        .pin_bit_mask = BIT64(ESP_VISION_CAMERA_POWER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        return ret;
    }

    int level = enable ? ESP_VISION_CAMERA_POWER_ON_LEVEL : !ESP_VISION_CAMERA_POWER_ON_LEVEL;
    ret = gpio_set_level((gpio_num_t)ESP_VISION_CAMERA_POWER_PIN, level);
    if ((ret == ESP_OK) && enable) {
        vTaskDelay(pdMS_TO_TICKS(ESP_VISION_CAMERA_POWER_STABLE_MS));
    }
    return ret;
#else
    (void)enable;
    return ESP_OK;
#endif
}

static int esp_vision_atom_s3r_m12_camera_init(void *config,
                                               int config_size,
                                               void **device_handle)
{
    (void)config;
    (void)config_size;

    esp_err_t ret = esp_vision_atom_s3r_m12_camera_set_power(true);
    if (ret != ESP_OK) {
        return ret;
    }

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

    ret = esp_camera_init(&camera_config);
    if (ret != ESP_OK) {
        (void)esp_vision_atom_s3r_m12_camera_set_power(false);
        return ret;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        (void)esp_camera_deinit();
        (void)esp_vision_atom_s3r_m12_camera_set_power(false);
        return ESP_FAIL;
    }

    *device_handle = sensor;
    return ESP_OK;
}

static int esp_vision_atom_s3r_m12_camera_deinit(void *device_handle)
{
    (void)device_handle;

    esp_err_t camera_ret = esp_camera_deinit();
    esp_err_t power_ret = esp_vision_atom_s3r_m12_camera_set_power(false);
    return (camera_ret != ESP_OK) ? camera_ret : power_ret;
}

CUSTOM_DEVICE_IMPLEMENT(camera,
                        esp_vision_atom_s3r_m12_camera_init,
                        esp_vision_atom_s3r_m12_camera_deinit);

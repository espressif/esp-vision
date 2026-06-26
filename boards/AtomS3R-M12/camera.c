/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "boardconfig.h"
#include "debug.h"
#include "jpeg.h"

/*
 * esp32-camera uses OpenMV-style PIXFORMAT_* names that conflict with imlib.
 * Keep those names local to this include and expose them with an ESP32_CAMERA_
 * prefix inside this board backend.
 */
#define pixformat_t         esp32_camera_pixformat_t
#define PIXFORMAT_RGB565    ESP32_CAMERA_PIXFORMAT_RGB565
#define PIXFORMAT_YUV422    ESP32_CAMERA_PIXFORMAT_YUV422
#define PIXFORMAT_YUV420    ESP32_CAMERA_PIXFORMAT_YUV420
#define PIXFORMAT_GRAYSCALE ESP32_CAMERA_PIXFORMAT_GRAYSCALE
#define PIXFORMAT_JPEG      ESP32_CAMERA_PIXFORMAT_JPEG
#define PIXFORMAT_RGB888    ESP32_CAMERA_PIXFORMAT_RGB888
#define PIXFORMAT_RAW       ESP32_CAMERA_PIXFORMAT_RAW
#define PIXFORMAT_RGB444    ESP32_CAMERA_PIXFORMAT_RGB444
#define PIXFORMAT_RGB555    ESP32_CAMERA_PIXFORMAT_RGB555
#define PIXFORMAT_RAW8      ESP32_CAMERA_PIXFORMAT_RAW8
#include "esp_camera.h"
#undef pixformat_t
#undef PIXFORMAT_RGB565
#undef PIXFORMAT_YUV422
#undef PIXFORMAT_YUV420
#undef PIXFORMAT_GRAYSCALE
#undef PIXFORMAT_JPEG
#undef PIXFORMAT_RGB888
#undef PIXFORMAT_RAW
#undef PIXFORMAT_RGB444
#undef PIXFORMAT_RGB555
#undef PIXFORMAT_RAW8

#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif

#ifndef NO_QSTR
#include "imlib.h"
#endif

#define ESP_VISION_CAMERA_CAPTURE_RETRY_COUNT 3
#define ESP_VISION_CAMERA_JPEG_QUALITY        12

typedef struct {
    bool initialized;
    bool hmirror;
    bool vflip;
    uint32_t raw_input_width;
    uint32_t raw_input_height;
    uint32_t active_input_width;
    uint32_t active_input_height;
    uint32_t active_input_offset_x;
    uint32_t active_input_offset_y;
    uint32_t width;
    uint32_t height;
    pixformat_t output_pixfmt;
} esp_vision_camera_context_t;

static const char *TAG = "esp_vision_camera";
static esp_vision_camera_context_t s_camera = {
    .width = ESP_VISION_CAMERA_OUTPUT_QVGA_WIDTH,
    .height = ESP_VISION_CAMERA_OUTPUT_QVGA_HEIGHT,
    .output_pixfmt = PIXFORMAT_RGB565,
};

static void esp_vision_camera_set_dimensions(uint32_t width, uint32_t height)
{
    s_camera.raw_input_width = width;
    s_camera.raw_input_height = height;
    s_camera.active_input_width = width;
    s_camera.active_input_height = height;
    s_camera.active_input_offset_x = 0;
    s_camera.active_input_offset_y = 0;
    s_camera.width = width;
    s_camera.height = height;
}

static void esp_vision_camera_set_defaults(void)
{
    esp_vision_camera_set_dimensions(ESP_VISION_CAMERA_OUTPUT_QVGA_WIDTH,
                                     ESP_VISION_CAMERA_OUTPUT_QVGA_HEIGHT);
    s_camera.output_pixfmt = PIXFORMAT_RGB565;
}

void esp_vision_camera_init0(void)
{
    esp_vision_camera_deinit();
    memset(&s_camera, 0, sizeof(s_camera));
    esp_vision_camera_set_defaults();
}

static size_t esp_vision_camera_bpp(uint32_t pixfmt)
{
    switch (pixfmt) {
    case PIXFORMAT_GRAYSCALE:
        return sizeof(uint8_t);
    case PIXFORMAT_RGB565:
        return sizeof(uint16_t);
    default:
        return 0;
    }
}

static size_t esp_vision_camera_output_size(uint32_t width, uint32_t height, uint32_t pixfmt)
{
    return (size_t)width * (size_t)height * esp_vision_camera_bpp(pixfmt);
}

static esp_err_t esp_vision_camera_set_power(bool enable)
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
    if (ret == ESP_OK && enable) {
        vTaskDelay(pdMS_TO_TICKS(ESP_VISION_CAMERA_POWER_STABLE_MS));
    }
    return ret;
#else
    (void)enable;
    return ESP_OK;
#endif
}

static esp_err_t esp_vision_camera_to_esp32_framesize(uint32_t width,
                                                      uint32_t height,
                                                      framesize_t *framesize)
{
    if (framesize == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((width == ESP_VISION_CAMERA_OUTPUT_QQVGA_WIDTH) &&
            (height == ESP_VISION_CAMERA_OUTPUT_QQVGA_HEIGHT)) {
        *framesize = FRAMESIZE_QQVGA;
        return ESP_OK;
    }

    if ((width == ESP_VISION_CAMERA_OUTPUT_QVGA_WIDTH) &&
            (height == ESP_VISION_CAMERA_OUTPUT_QVGA_HEIGHT)) {
        *framesize = FRAMESIZE_QVGA;
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static void esp_vision_camera_log_jpeg_frame(const char *reason, const camera_fb_t *fb)
{
    if ((fb == NULL) || (fb->buf == NULL) || (fb->len == 0)) {
        esp_vision_debug_printf("[esp-vision] camera jpeg %s: no frame\r\n", reason);
        return;
    }

    const uint8_t *data = fb->buf;
    size_t size = fb->len;
    uint8_t h0 = (size > 0) ? data[0] : 0;
    uint8_t h1 = (size > 1) ? data[1] : 0;
    uint8_t h2 = (size > 2) ? data[2] : 0;
    uint8_t h3 = (size > 3) ? data[3] : 0;
    uint8_t t0 = (size > 1) ? data[size - 2] : 0;
    uint8_t t1 = (size > 0) ? data[size - 1] : 0;

    esp_vision_debug_printf("[esp-vision] camera jpeg %s: bytes=%u frame=%ux%u fmt=%u"
                            " head=%02x %02x %02x %02x tail=%02x %02x\r\n",
                            reason,
                            (unsigned int)fb->len,
                            (unsigned int)fb->width,
                            (unsigned int)fb->height,
                            (unsigned int)fb->format,
                            h0,
                            h1,
                            h2,
                            h3,
                            t0,
                            t1);
}

static bool esp_vision_camera_get_jpeg_payload(const camera_fb_t *fb, image_t *src)
{
    if ((fb == NULL) || (src == NULL) || (fb->buf == NULL) || (fb->len < 4)) {
        return false;
    }

    size_t start = fb->len;
    size_t end = fb->len;

    for (size_t i = 0; (i + 1) < fb->len; i++) {
        if ((fb->buf[i] == 0xff) && (fb->buf[i + 1] == 0xd8)) {
            start = i;
            break;
        }
    }

    if (start == fb->len) {
        esp_vision_camera_log_jpeg_frame("missing soi", fb);
        return false;
    }

    for (size_t i = start + 2; (i + 1) < fb->len; i++) {
        if ((fb->buf[i] == 0xff) && (fb->buf[i + 1] == 0xd9)) {
            end = i + 2;
            break;
        }
    }

    static bool logged_adjustment = false;
    if (!logged_adjustment && ((start != 0) || (end != fb->len))) {
        esp_vision_debug_printf("[esp-vision] camera jpeg payload: offset=%u size=%u frame_bytes=%u\r\n",
                                (unsigned int)start,
                                (unsigned int)(end - start),
                                (unsigned int)fb->len);
        logged_adjustment = true;
    }

    *src = (image_t) {
        .w = (int32_t)fb->width,
        .h = (int32_t)fb->height,
        .pixfmt = PIXFORMAT_JPEG,
        .size = end - start,
        .pixels = fb->buf + start,
    };
    return true;
}

esp_err_t esp_vision_camera_get_framesize_dimensions(esp_vision_camera_framesize_t framesize,
                                                     uint32_t *width,
                                                     uint32_t *height)
{
    if ((width == NULL) || (height == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (framesize) {
    case ESP_VISION_CAMERA_FRAMESIZE_QQVGA:
        *width = ESP_VISION_CAMERA_OUTPUT_QQVGA_WIDTH;
        *height = ESP_VISION_CAMERA_OUTPUT_QQVGA_HEIGHT;
        return ESP_OK;
    case ESP_VISION_CAMERA_FRAMESIZE_QVGA:
        *width = ESP_VISION_CAMERA_OUTPUT_QVGA_WIDTH;
        *height = ESP_VISION_CAMERA_OUTPUT_QVGA_HEIGHT;
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t esp_vision_camera_init(void)
{
    if (s_camera.initialized) {
        return ESP_OK;
    }

    if ((s_camera.width == 0) || (s_camera.height == 0) || (s_camera.output_pixfmt == PIXFORMAT_INVALID)) {
        esp_vision_camera_set_defaults();
    }

    framesize_t frame_size = FRAMESIZE_QVGA;
    esp_err_t ret = esp_vision_camera_to_esp32_framesize(s_camera.width, s_camera.height, &frame_size);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_vision_camera_set_power(true);
    if (ret != ESP_OK) {
        esp_vision_debug_printf("[esp-vision] camera power on failed: %s\r\n", esp_err_to_name(ret));
        return ret;
    }

    const camera_config_t config = {
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
        .pixel_format = ESP32_CAMERA_PIXFORMAT_JPEG,
        .frame_size = frame_size,
        .jpeg_quality = ESP_VISION_CAMERA_JPEG_QUALITY,
        .fb_count = ESP_VISION_CAMERA_BUFFER_COUNT,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = ESP_VISION_CAMERA_SCCB_I2C_PORT,
    };

    ret = esp_camera_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize esp32-camera: %s", esp_err_to_name(ret));
        esp_vision_debug_printf("[esp-vision] camera start failed: %s\r\n", esp_err_to_name(ret));
        esp_vision_camera_set_power(false);
        return ret;
    }

    s_camera.vflip = false;
    s_camera.hmirror = true;
    s_camera.initialized = true;

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_hmirror(sensor, s_camera.hmirror ? 1 : 0);
        sensor->set_vflip(sensor, s_camera.vflip ? 1 : 0);
    }

    esp_vision_debug_printf("[esp-vision] camera started: sensor=0x%04" PRIx32
                            " raw=%" PRIu32 "x%" PRIu32
                            " active=%" PRIu32 "x%" PRIu32 "+%" PRIu32 "+%" PRIu32
                            " output=%" PRIu32 "x%" PRIu32
                            " input=jpeg driver=esp32-camera pixfmt=%" PRIu32 "\r\n",
                            ESP_VISION_CAMERA_SENSOR_ID,
                            s_camera.raw_input_width,
                            s_camera.raw_input_height,
                            s_camera.active_input_width,
                            s_camera.active_input_height,
                            s_camera.active_input_offset_x,
                            s_camera.active_input_offset_y,
                            s_camera.width,
                            s_camera.height,
                            (uint32_t)s_camera.output_pixfmt);
    return ESP_OK;
}

void esp_vision_camera_deinit(void)
{
    if (s_camera.initialized) {
        esp_camera_deinit();
        s_camera.initialized = false;
    }
    esp_vision_camera_set_power(false);
}

bool esp_vision_camera_is_ready(void)
{
    return s_camera.initialized &&
           (s_camera.width != 0) &&
           (s_camera.height != 0);
}

esp_err_t esp_vision_camera_set_pixformat(uint32_t pixfmt)
{
    if ((pixfmt != PIXFORMAT_RGB565) && (pixfmt != PIXFORMAT_GRAYSCALE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_camera.output_pixfmt = (pixformat_t)pixfmt;
    return ESP_OK;
}

uint32_t esp_vision_camera_get_pixformat(void)
{
    return s_camera.output_pixfmt;
}

esp_err_t esp_vision_camera_set_framesize(esp_vision_camera_framesize_t framesize)
{
    uint32_t width = 0;
    uint32_t height = 0;
    esp_err_t ret = esp_vision_camera_get_framesize_dimensions(framesize, &width, &height);
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_camera.initialized) {
        framesize_t esp32_framesize = FRAMESIZE_QVGA;
        ret = esp_vision_camera_to_esp32_framesize(width, height, &esp32_framesize);
        if (ret != ESP_OK) {
            return ret;
        }

        sensor_t *sensor = esp_camera_sensor_get();
        if ((sensor == NULL) || (sensor->set_framesize == NULL)) {
            return ESP_FAIL;
        }
        if (sensor->set_framesize(sensor, esp32_framesize) != 0) {
            return ESP_FAIL;
        }
    }

    esp_vision_camera_set_dimensions(width, height);
    return ESP_OK;
}

uint32_t esp_vision_camera_get_width(void)
{
    return s_camera.width;
}

uint32_t esp_vision_camera_get_height(void)
{
    return s_camera.height;
}

esp_err_t esp_vision_camera_set_hmirror(bool enable)
{
    s_camera.hmirror = enable;

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_hmirror(sensor, enable ? 1 : 0);
    }
    return ESP_OK;
}

bool esp_vision_camera_get_hmirror(void)
{
    return s_camera.hmirror;
}

esp_err_t esp_vision_camera_set_vflip(bool enable)
{
    s_camera.vflip = enable;

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_vflip(sensor, enable ? 1 : 0);
    }
    return ESP_OK;
}

bool esp_vision_camera_get_vflip(void)
{
    return s_camera.vflip;
}

uint32_t esp_vision_camera_get_sensor_id(void)
{
    return ESP_VISION_CAMERA_SENSOR_ID;
}

size_t esp_vision_camera_frame_size(void)
{
    return esp_vision_camera_output_size(s_camera.width, s_camera.height, s_camera.output_pixfmt);
}

esp_err_t esp_vision_camera_capture(uint8_t *pixels, size_t pixels_size)
{
    if ((pixels == NULL) || !esp_vision_camera_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t expected_size = esp_vision_camera_frame_size();
    if ((expected_size == 0) || (pixels_size < expected_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (int attempt = 0; attempt < ESP_VISION_CAMERA_CAPTURE_RETRY_COUNT; attempt++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            return ESP_FAIL;
        }

        esp_err_t ret = ESP_OK;
        image_t src;
        image_t dst = {
            .w = (int32_t)s_camera.width,
            .h = (int32_t)s_camera.height,
            .pixfmt = s_camera.output_pixfmt,
            .size = 0,
            .pixels = pixels,
        };

        if ((fb->format != ESP32_CAMERA_PIXFORMAT_JPEG) ||
                !esp_vision_camera_get_jpeg_payload(fb, &src)) {
            esp_vision_camera_log_jpeg_frame("invalid", fb);
            ret = ESP_ERR_INVALID_RESPONSE;
        } else if (((uint32_t)src.w != s_camera.width) || ((uint32_t)src.h != s_camera.height)) {
            esp_vision_camera_log_jpeg_frame("unexpected size", fb);
            ret = ESP_ERR_INVALID_RESPONSE;
        } else {
            ret = esp_vision_jpeg_decode(src.pixels, src.size, &dst);
            if (ret != ESP_OK) {
                esp_vision_camera_log_jpeg_frame("decode failed", fb);
            }
        }

        esp_camera_fb_return(fb);
        if (ret != ESP_ERR_INVALID_RESPONSE) {
            return ret;
        }
    }

    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t esp_vision_camera_snapshot(image_t *img, uint8_t *pixels, size_t pixels_size)
{
    if (img == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_vision_camera_capture(pixels, pixels_size);
    if (ret != ESP_OK) {
        return ret;
    }

    img->w = s_camera.width;
    img->h = s_camera.height;
    img->pixfmt = s_camera.output_pixfmt;
    img->size = 0;
    img->_raw = NULL;
    img->pixels = pixels;
    return ESP_OK;
}

esp_err_t esp_vision_camera_get_status(esp_vision_camera_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    status->ready = esp_vision_camera_is_ready();
    status->sensor_id = ESP_VISION_CAMERA_SENSOR_ID;
    status->raw_input_width = s_camera.raw_input_width;
    status->raw_input_height = s_camera.raw_input_height;
    status->active_input_width = s_camera.active_input_width;
    status->active_input_height = s_camera.active_input_height;
    status->active_input_offset_x = s_camera.active_input_offset_x;
    status->active_input_offset_y = s_camera.active_input_offset_y;
    status->width = s_camera.width;
    status->height = s_camera.height;
    status->pixfmt = s_camera.output_pixfmt;
    status->hmirror = s_camera.hmirror;
    status->vflip = s_camera.vflip;
    return ESP_OK;
}

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera.h"

#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "sys/mman.h"

#include "boardconfig.h"
#include "debug.h"

#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif

#ifndef NO_QSTR
#include "imlib.h"
#endif

#define ESP_VISION_CAMERA_MEMORY_TYPE V4L2_MEMORY_MMAP
#define ESP_VISION_CAMERA_OV3660_SCCB_ADDR 0x3c
#define ESP_VISION_CAMERA_OV3660_SENSOR_ID 0x3660
#define ESP_VISION_CAMERA_OV3660_ID_HIGH_REG 0x300a
#define ESP_VISION_CAMERA_OV3660_ID_LOW_REG 0x300b
#define ESP_VISION_CAMERA_OV3660_SYSTEM_CONTROL0 0x3008
#define ESP_VISION_CAMERA_OV3660_SOFT_RESET_VALUE 0x82
#define ESP_VISION_CAMERA_OV3660_SOFT_RESET_DELAY_MS 20
#define ESP_VISION_CAMERA_SC101IOT_SCCB_ADDR 0x68
#define ESP_VISION_CAMERA_SC101IOT_SENSOR_ID 0xda4a
#define ESP_VISION_CAMERA_SC101IOT_ID_HIGH_REG 0x31f7
#define ESP_VISION_CAMERA_SC101IOT_ID_LOW_REG 0x31f8
#define ESP_VISION_CAMERA_SC101IOT_PAGE_SELECT_REG 0xf0
#define ESP_VISION_CAMERA_SC101IOT_RAW_INPUT_WIDTH 1280
#define ESP_VISION_CAMERA_SC101IOT_RAW_INPUT_HEIGHT 720
#define ESP_VISION_CAMERA_SCCB_TIMEOUT_MS 100
#define ESP_VISION_CAMERA_DQBUF_TIMEOUT_MS 500
#define ESP_VISION_CAMERA_DQBUF_RESTART_MAX 1
#define ESP_VISION_CAMERA_STREAM_PRIME_ATTEMPTS 3
#define ESP_VISION_CAMERA_FULL_INIT_ATTEMPTS 3
#define ESP_VISION_CAMERA_INIT_RETRY_DELAY_MS 100

typedef enum {
    ESP_VISION_CAMERA_SENSOR_UNKNOWN = 0,
    ESP_VISION_CAMERA_SENSOR_OV3660,
    ESP_VISION_CAMERA_SENSOR_SC101IOT,
} esp_vision_camera_sensor_t;

typedef struct {
    void *ptr;
    size_t len;
} esp_vision_camera_buffer_t;

typedef struct {
    bool initialized;
    bool video_initialized;
    bool xclk_started;
    bool streaming;
    bool hmirror;
    bool vflip;
    i2c_master_bus_handle_t i2c_handle;
    int fd;
    esp_vision_camera_sensor_t sensor;
    uint32_t sensor_id;
    uint32_t raw_input_width;
    uint32_t raw_input_height;
    uint32_t active_input_width;
    uint32_t active_input_height;
    uint32_t active_input_offset_x;
    uint32_t active_input_offset_y;
    uint32_t width;
    uint32_t height;
    uint32_t input_pixfmt;
    uint32_t input_stride;
    uint32_t buffer_count;
    uint32_t dqbuf_restart_count;
    pixformat_t output_pixfmt;
    size_t ppa_out_size;
    ppa_client_handle_t ppa_handle;
    uint8_t *ppa_out_buf;
    esp_vision_camera_buffer_t buffers[ESP_VISION_CAMERA_BUFFER_COUNT];
} esp_vision_camera_context_t;

static const char *TAG = "esp_vision_camera";
static esp_vision_camera_context_t s_camera = {
    .fd = -1,
    .vflip = true,
    .sensor_id = ESP_VISION_CAMERA_SENSOR_ID,
    .width = ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_WIDTH,
    .height = ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_HEIGHT,
    .output_pixfmt = PIXFORMAT_RGB565,
};

static void esp_vision_camera_set_defaults(void)
{
    s_camera.fd = -1;
    s_camera.vflip = true;
    s_camera.sensor = ESP_VISION_CAMERA_SENSOR_UNKNOWN;
    s_camera.sensor_id = ESP_VISION_CAMERA_SENSOR_ID;
    s_camera.width = ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_WIDTH;
    s_camera.height = ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_HEIGHT;
    s_camera.output_pixfmt = PIXFORMAT_RGB565;
}

void esp_vision_camera_init0(void)
{
    esp_vision_camera_deinit();
    memset(&s_camera, 0, sizeof(s_camera));
    esp_vision_camera_set_defaults();
}

static size_t esp_vision_camera_align_up(size_t value, size_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    return ((value + alignment - 1) / alignment) * alignment;
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

static ppa_srm_color_mode_t esp_vision_camera_output_color_mode(uint32_t pixfmt)
{
    return (pixfmt == PIXFORMAT_GRAYSCALE) ? PPA_SRM_COLOR_MODE_GRAY8 : PPA_SRM_COLOR_MODE_RGB565;
}

static const char *esp_vision_camera_input_name(void)
{
    switch (s_camera.input_pixfmt) {
    case V4L2_PIX_FMT_RGB565X:
        return "rgb565be";
    case V4L2_PIX_FMT_RGB565:
        return "rgb565le";
    case V4L2_PIX_FMT_YUYV:
        return "yuyv";
    default:
        return "unknown";
    }
}

static esp_err_t esp_vision_camera_configure_sccb_pullups(void)
{
#if ESP_VISION_CAMERA_SCCB_INTERNAL_PULLUP
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(ESP_VISION_CAMERA_SCCB_I2C_SCL_PIN, GPIO_PULLUP_ONLY),
                        TAG,
                        "failed to enable SCCB SCL pull-up");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(ESP_VISION_CAMERA_SCCB_I2C_SDA_PIN, GPIO_PULLUP_ONLY),
                        TAG,
                        "failed to enable SCCB SDA pull-up");
#endif
    return ESP_OK;
}

static esp_err_t esp_vision_camera_i2c_init(void)
{
    if (s_camera.i2c_handle != NULL) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vision_camera_configure_sccb_pullups();
    if (ret != ESP_OK) {
        esp_vision_debug_printf("[esp-vision] camera init: configure SCCB pull-ups failed ret=%d\r\n", (int)ret);
        return ret;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = ESP_VISION_CAMERA_SCCB_I2C_PORT,
        .sda_io_num = ESP_VISION_CAMERA_SCCB_I2C_SDA_PIN,
        .scl_io_num = ESP_VISION_CAMERA_SCCB_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = ESP_VISION_CAMERA_SCCB_INTERNAL_PULLUP,
        },
    };

    ret = i2c_new_master_bus(&bus_config, &s_camera.i2c_handle);
    if (ret != ESP_OK) {
        esp_vision_debug_printf("[esp-vision] camera init: create SCCB I2C bus failed ret=%d\r\n", (int)ret);
        return ret;
    }
    return ESP_OK;
}

static void esp_vision_camera_i2c_deinit(void)
{
    if (s_camera.i2c_handle == NULL) {
        return;
    }

    esp_err_t ret = i2c_del_master_bus(s_camera.i2c_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "failed to delete camera SCCB I2C bus: %s", esp_err_to_name(ret));
    }
    s_camera.i2c_handle = NULL;
}

static esp_err_t esp_vision_camera_sccb_read_reg16(uint8_t addr, uint16_t reg, uint8_t *value)
{
    if ((s_camera.i2c_handle == NULL) || (value == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = ESP_VISION_CAMERA_SCCB_I2C_FREQ,
    };

    esp_err_t ret = i2c_master_bus_add_device(s_camera.i2c_handle, &device_config, &device);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint8_t reg_addr[] = {
        (uint8_t)(reg >> 8),
        (uint8_t)reg,
    };
    ret = i2c_master_transmit_receive(device,
                                      reg_addr,
                                      sizeof(reg_addr),
                                      value,
                                      sizeof(*value),
                                      ESP_VISION_CAMERA_SCCB_TIMEOUT_MS);

    esp_err_t del_ret = i2c_master_bus_rm_device(device);
    return (ret != ESP_OK) ? ret : del_ret;
}

static esp_err_t esp_vision_camera_sc101iot_read_reg(uint16_t reg, uint8_t *value)
{
    if ((s_camera.i2c_handle == NULL) || (value == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ESP_VISION_CAMERA_SC101IOT_SCCB_ADDR,
        .scl_speed_hz = ESP_VISION_CAMERA_SCCB_I2C_FREQ,
    };

    esp_err_t ret = i2c_master_bus_add_device(s_camera.i2c_handle, &device_config, &device);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint8_t page_addr[] = {
        ESP_VISION_CAMERA_SC101IOT_PAGE_SELECT_REG,
        (uint8_t)(reg >> 8),
    };
    ret = i2c_master_transmit(device,
                              page_addr,
                              sizeof(page_addr),
                              ESP_VISION_CAMERA_SCCB_TIMEOUT_MS);
    if (ret == ESP_OK) {
        const uint8_t reg_addr = (uint8_t)reg;
        ret = i2c_master_transmit_receive(device,
                                          &reg_addr,
                                          sizeof(reg_addr),
                                          value,
                                          sizeof(*value),
                                          ESP_VISION_CAMERA_SCCB_TIMEOUT_MS);
    }

    esp_err_t del_ret = i2c_master_bus_rm_device(device);
    return (ret != ESP_OK) ? ret : del_ret;
}

static esp_err_t esp_vision_camera_read_sensor_id(uint8_t addr, uint16_t high_reg, uint16_t low_reg, uint32_t *id)
{
    uint8_t high = 0;
    uint8_t low = 0;

    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_vision_camera_sccb_read_reg16(addr, high_reg, &high);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_vision_camera_sccb_read_reg16(addr, low_reg, &low);
    if (ret != ESP_OK) {
        return ret;
    }

    *id = ((uint32_t)high << 8) | low;
    return ESP_OK;
}

static esp_err_t esp_vision_camera_read_sc101iot_sensor_id(uint32_t *id)
{
    uint8_t high = 0;
    uint8_t low = 0;

    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_vision_camera_sc101iot_read_reg(ESP_VISION_CAMERA_SC101IOT_ID_HIGH_REG, &high);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_vision_camera_sc101iot_read_reg(ESP_VISION_CAMERA_SC101IOT_ID_LOW_REG, &low);
    if (ret != ESP_OK) {
        return ret;
    }

    *id = ((uint32_t)high << 8) | low;
    return ESP_OK;
}

static esp_err_t esp_vision_camera_probe_sensor(void)
{
    uint32_t id = 0;
    esp_err_t ov_ret = esp_vision_camera_read_sensor_id(ESP_VISION_CAMERA_OV3660_SCCB_ADDR,
                                                        ESP_VISION_CAMERA_OV3660_ID_HIGH_REG,
                                                        ESP_VISION_CAMERA_OV3660_ID_LOW_REG,
                                                        &id);
    if ((ov_ret == ESP_OK) && (id == ESP_VISION_CAMERA_OV3660_SENSOR_ID)) {
        s_camera.sensor = ESP_VISION_CAMERA_SENSOR_OV3660;
        s_camera.sensor_id = id;
        esp_vision_debug_printf("[esp-vision] camera init: probe sensor=OV3660 id=0x%04" PRIx32 "\r\n", id);
        return ESP_OK;
    }

    uint32_t sc_ret_id = 0;
    esp_err_t sc_ret = esp_vision_camera_read_sc101iot_sensor_id(&sc_ret_id);
    if ((sc_ret == ESP_OK) && (sc_ret_id == ESP_VISION_CAMERA_SC101IOT_SENSOR_ID)) {
        s_camera.sensor = ESP_VISION_CAMERA_SENSOR_SC101IOT;
        s_camera.sensor_id = sc_ret_id;
        esp_vision_debug_printf("[esp-vision] camera init: probe sensor=SC101IOT id=0x%04" PRIx32 "\r\n", sc_ret_id);
        return ESP_OK;
    }

    esp_vision_debug_printf("[esp-vision] camera init: probe sensor failed ov_ret=%d ov_id=0x%04" PRIx32
                            " sc_ret=%d sc_id=0x%04" PRIx32 "\r\n",
                            (int)ov_ret,
                            id,
                            (int)sc_ret,
                            sc_ret_id);
    s_camera.sensor = ESP_VISION_CAMERA_SENSOR_UNKNOWN;
    s_camera.sensor_id = ESP_VISION_CAMERA_SENSOR_ID;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t esp_vision_camera_ov3660_soft_reset(void)
{
    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ESP_VISION_CAMERA_OV3660_SCCB_ADDR,
        .scl_speed_hz = ESP_VISION_CAMERA_SCCB_I2C_FREQ,
    };

    esp_err_t ret = i2c_master_bus_add_device(s_camera.i2c_handle, &device_config, &device);
    if (ret != ESP_OK) {
        esp_vision_debug_printf("[esp-vision] camera init: add OV3660 SCCB device failed ret=%d\r\n", (int)ret);
        return ret;
    }

    const uint8_t tx_data[] = {
        (uint8_t)(ESP_VISION_CAMERA_OV3660_SYSTEM_CONTROL0 >> 8),
        (uint8_t)ESP_VISION_CAMERA_OV3660_SYSTEM_CONTROL0,
        ESP_VISION_CAMERA_OV3660_SOFT_RESET_VALUE,
    };
    ret = i2c_master_transmit(device, tx_data, sizeof(tx_data), ESP_VISION_CAMERA_SCCB_TIMEOUT_MS);
    esp_err_t del_ret = i2c_master_bus_rm_device(device);
    if (ret != ESP_OK) {
        esp_vision_debug_printf("[esp-vision] camera init: write OV3660 soft reset failed ret=%d\r\n", (int)ret);
        return ret;
    }
    if (del_ret != ESP_OK) {
        esp_vision_debug_printf("[esp-vision] camera init: remove OV3660 SCCB device failed ret=%d\r\n", (int)del_ret);
        return del_ret;
    }

    vTaskDelay(pdMS_TO_TICKS(ESP_VISION_CAMERA_OV3660_SOFT_RESET_DELAY_MS));
    return ESP_OK;
}

static esp_err_t esp_vision_camera_start_xclk(void)
{
    if (s_camera.xclk_started) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = (ledc_timer_t)ESP_VISION_CAMERA_XCLK_LEDC_TIMER,
        .freq_hz = ESP_VISION_CAMERA_XCLK_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel_config = {
        .gpio_num = ESP_VISION_CAMERA_XCLK_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)ESP_VISION_CAMERA_XCLK_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)ESP_VISION_CAMERA_XCLK_LEDC_TIMER,
        .duty = 1,
        .hpoint = 0,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "failed to configure camera XCLK timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), TAG, "failed to configure camera XCLK channel");

    s_camera.xclk_started = true;
    if (ESP_VISION_CAMERA_XCLK_STABLE_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(ESP_VISION_CAMERA_XCLK_STABLE_MS));
    }
    return ESP_OK;
}

static void esp_vision_camera_stop_xclk(void)
{
    if (!s_camera.xclk_started) {
        return;
    }

    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ESP_VISION_CAMERA_XCLK_LEDC_CHANNEL, 0);
    ledc_timer_pause(LEDC_LOW_SPEED_MODE, (ledc_timer_t)ESP_VISION_CAMERA_XCLK_LEDC_TIMER);

    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = (ledc_timer_t)ESP_VISION_CAMERA_XCLK_LEDC_TIMER,
        .deconfigure = true,
    };
    ledc_timer_config(&timer_config);
    s_camera.xclk_started = false;
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
        *width = ESP_VISION_CAMERA_PPA_OUTPUT_QQVGA_WIDTH;
        *height = ESP_VISION_CAMERA_PPA_OUTPUT_QQVGA_HEIGHT;
        return ESP_OK;
    case ESP_VISION_CAMERA_FRAMESIZE_QVGA:
        *width = ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_WIDTH;
        *height = ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_HEIGHT;
        return ESP_OK;
    case ESP_VISION_CAMERA_FRAMESIZE_VGA:
        *width = ESP_VISION_CAMERA_PPA_OUTPUT_VGA_WIDTH;
        *height = ESP_VISION_CAMERA_PPA_OUTPUT_VGA_HEIGHT;
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static void esp_vision_camera_release_buffers(void)
{
    for (size_t i = 0; i < ESP_VISION_CAMERA_BUFFER_COUNT; i++) {
        if (s_camera.buffers[i].ptr != NULL) {
            munmap(s_camera.buffers[i].ptr, s_camera.buffers[i].len);
            s_camera.buffers[i].ptr = NULL;
            s_camera.buffers[i].len = 0;
        }
    }
}

static void esp_vision_camera_release_ppa(void)
{
    if (s_camera.ppa_handle != NULL) {
        ppa_unregister_client(s_camera.ppa_handle);
        s_camera.ppa_handle = NULL;
    }
    if (s_camera.ppa_out_buf != NULL) {
        heap_caps_free(s_camera.ppa_out_buf);
        s_camera.ppa_out_buf = NULL;
    }
    s_camera.ppa_out_size = 0;
}

static esp_err_t esp_vision_camera_video_init(void)
{
#if CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE
    esp_cam_ctlr_dvp_pin_config_t dvp_pin = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = {
            ESP_VISION_CAMERA_DVP_D0_PIN,
            ESP_VISION_CAMERA_DVP_D1_PIN,
            ESP_VISION_CAMERA_DVP_D2_PIN,
            ESP_VISION_CAMERA_DVP_D3_PIN,
            ESP_VISION_CAMERA_DVP_D4_PIN,
            ESP_VISION_CAMERA_DVP_D5_PIN,
            ESP_VISION_CAMERA_DVP_D6_PIN,
            ESP_VISION_CAMERA_DVP_D7_PIN,
        },
        .vsync_io = ESP_VISION_CAMERA_DVP_VSYNC_PIN,
        .de_io = ESP_VISION_CAMERA_DVP_HSYNC_PIN,
        .pclk_io = ESP_VISION_CAMERA_DVP_PCLK_PIN,
        .xclk_io = GPIO_NUM_NC,
    };
    esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = {
            .init_sccb = false,
            .freq = ESP_VISION_CAMERA_SCCB_I2C_FREQ,
        },
        .reset_pin = ESP_VISION_CAMERA_SENSOR_RESET_PIN,
        .pwdn_pin = ESP_VISION_CAMERA_SENSOR_PWDN_PIN,
        .dvp_pin = dvp_pin,
        .xclk_freq = 0,
    };
    const esp_video_init_config_t video_config = {
        .dvp = &dvp_config,
    };

    esp_err_t ret = esp_vision_camera_start_xclk();
    if (ret != ESP_OK) {
        esp_vision_debug_printf("[esp-vision] camera init: start xclk failed ret=%d\r\n", (int)ret);
        return ret;
    }

    ret = esp_vision_camera_i2c_init();
    if (ret != ESP_OK) {
        esp_vision_debug_printf("[esp-vision] camera init: sccb i2c failed ret=%d\r\n", (int)ret);
        return ret;
    }

    ret = esp_vision_camera_probe_sensor();
    if (ret != ESP_OK) {
        esp_vision_debug_printf("[esp-vision] camera init: sensor probe failed ret=%d\r\n", (int)ret);
        return ret;
    }

    if (s_camera.sensor == ESP_VISION_CAMERA_SENSOR_OV3660) {
        ret = esp_vision_camera_ov3660_soft_reset();
        if (ret != ESP_OK) {
            esp_vision_debug_printf("[esp-vision] camera init: ov3660 soft reset failed ret=%d\r\n", (int)ret);
            return ret;
        }
        esp_vision_debug_printf("[esp-vision] camera init: ov3660 soft reset ok\r\n");
    }

    dvp_config.sccb_config.i2c_handle = s_camera.i2c_handle;
    ret = esp_video_init_with_flags(&video_config, ESP_VIDEO_INIT_FLAGS_DVP);
    if (ret != ESP_OK) {
        esp_vision_debug_printf("[esp-vision] camera init: esp-video DVP failed ret=%d\r\n", (int)ret);
        return ret;
    }
    s_camera.video_initialized = true;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void esp_vision_camera_video_deinit(void)
{
    if (s_camera.video_initialized) {
        esp_video_deinit_with_flags(ESP_VIDEO_INIT_FLAGS_DVP);
        s_camera.video_initialized = false;
    }
    esp_vision_camera_i2c_deinit();
    esp_vision_camera_stop_xclk();
}

static esp_err_t esp_vision_camera_open_device(void)
{
    struct v4l2_capability capability = {0};

    s_camera.fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
    if (s_camera.fd < 0) {
        ESP_LOGE(TAG, "failed to open %s", ESP_VIDEO_DVP_DEVICE_NAME);
        return ESP_FAIL;
    }

    if (ioctl(s_camera.fd, VIDIOC_QUERYCAP, &capability) != 0) {
        ESP_LOGE(TAG, "failed to query camera capability");
        close(s_camera.fd);
        s_camera.fd = -1;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t esp_vision_camera_set_dqbuf_timeout(void)
{
    struct timeval timeout = {
        .tv_sec = ESP_VISION_CAMERA_DQBUF_TIMEOUT_MS / 1000,
        .tv_usec = (ESP_VISION_CAMERA_DQBUF_TIMEOUT_MS % 1000) * 1000,
    };

    if (ioctl(s_camera.fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout) != 0) {
        esp_vision_debug_printf("[esp-vision] camera init: set dqbuf timeout failed\r\n");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool esp_vision_camera_is_rgb565_input(uint32_t pixfmt)
{
    return (pixfmt == V4L2_PIX_FMT_RGB565X) || (pixfmt == V4L2_PIX_FMT_RGB565);
}

static bool esp_vision_camera_is_supported_input(uint32_t pixfmt)
{
    return esp_vision_camera_is_rgb565_input(pixfmt) || (pixfmt == V4L2_PIX_FMT_YUYV);
}

static esp_err_t esp_vision_camera_configure_input_format(uint32_t width, uint32_t height, uint32_t pixfmt)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = width,
        .fmt.pix.height = height,
        .fmt.pix.pixelformat = pixfmt,
    };

    if (ioctl(s_camera.fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "failed to set camera input format");
        return ESP_FAIL;
    }

    if (!esp_vision_camera_is_supported_input(format.fmt.pix.pixelformat)) {
        ESP_LOGE(TAG, "camera selected unsupported input format: 0x%08" PRIx32, format.fmt.pix.pixelformat);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((pixfmt == V4L2_PIX_FMT_YUYV) && (format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)) {
        ESP_LOGE(TAG, "camera selected non-YUYV input format: 0x%08" PRIx32, format.fmt.pix.pixelformat);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (esp_vision_camera_is_rgb565_input(pixfmt) && !esp_vision_camera_is_rgb565_input(format.fmt.pix.pixelformat)) {
        ESP_LOGE(TAG, "camera selected non-RGB565 input format: 0x%08" PRIx32, format.fmt.pix.pixelformat);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t bytesperline = format.fmt.pix.bytesperline;
    if (bytesperline == 0) {
        bytesperline = format.fmt.pix.width * sizeof(uint16_t);
    }
    if (bytesperline != (format.fmt.pix.width * sizeof(uint16_t))) {
        ESP_LOGE(TAG, "camera stride is unsupported: %" PRIu32, format.fmt.pix.bytesperline);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_camera.raw_input_width = format.fmt.pix.width;
    s_camera.raw_input_height = format.fmt.pix.height;
    s_camera.input_pixfmt = format.fmt.pix.pixelformat;
    s_camera.input_stride = bytesperline;
    return ESP_OK;
}

static esp_err_t esp_vision_camera_set_input_format(void)
{
    if (s_camera.sensor == ESP_VISION_CAMERA_SENSOR_SC101IOT) {
        return esp_vision_camera_configure_input_format(ESP_VISION_CAMERA_SC101IOT_RAW_INPUT_WIDTH,
                                                        ESP_VISION_CAMERA_SC101IOT_RAW_INPUT_HEIGHT,
                                                        V4L2_PIX_FMT_YUYV);
    }

    return esp_vision_camera_configure_input_format(ESP_VISION_CAMERA_RAW_INPUT_WIDTH,
                                                    ESP_VISION_CAMERA_RAW_INPUT_HEIGHT,
                                                    V4L2_PIX_FMT_RGB565X);
}

static esp_err_t esp_vision_camera_queue_index(uint32_t index)
{
    if (index >= s_camera.buffer_count) {
        return ESP_ERR_INVALID_ARG;
    }

    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = ESP_VISION_CAMERA_MEMORY_TYPE,
        .index = index,
    };

    return (ioctl(s_camera.fd, VIDIOC_QBUF, &buf) == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t esp_vision_camera_queue_all_buffers(void)
{
    for (uint32_t i = 0; i < s_camera.buffer_count; i++) {
        esp_err_t ret = esp_vision_camera_queue_index(i);
        if (ret != ESP_OK) {
            esp_vision_debug_printf("[esp-vision] camera stream: qbuf index=%" PRIu32 " failed ret=%d\r\n",
                                    i,
                                    (int)ret);
            return ret;
        }
    }
    return ESP_OK;
}

static void esp_vision_camera_update_active_window(void)
{
    s_camera.active_input_width = ESP_VISION_CAMERA_ACTIVE_INPUT_WIDTH;
    s_camera.active_input_height = ESP_VISION_CAMERA_ACTIVE_INPUT_HEIGHT;

    if ((s_camera.active_input_width == 0) || (s_camera.active_input_width > s_camera.raw_input_width)) {
        s_camera.active_input_width = s_camera.raw_input_width;
    }
    if ((s_camera.active_input_height == 0) || (s_camera.active_input_height > s_camera.raw_input_height)) {
        s_camera.active_input_height = s_camera.raw_input_height;
    }

    s_camera.active_input_offset_x = (s_camera.raw_input_width - s_camera.active_input_width) / 2;
    s_camera.active_input_offset_y = (s_camera.raw_input_height - s_camera.active_input_height) / 2;
}

static esp_err_t esp_vision_camera_init_ppa(void)
{
    esp_err_t ret;
    size_t cache_align = ESP_VISION_DMA_ALIGNMENT;
    size_t out_size;
    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
    };

    out_size = esp_vision_camera_output_size(s_camera.width, s_camera.height, s_camera.output_pixfmt);
    if (out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = ppa_register_client(&ppa_config, &s_camera.ppa_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to register PPA client");

    s_camera.ppa_out_size = esp_vision_camera_align_up(out_size, cache_align);
    s_camera.ppa_out_buf = heap_caps_aligned_calloc(cache_align,
                                                    1,
                                                    s_camera.ppa_out_size,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_camera.ppa_out_buf == NULL) {
        s_camera.ppa_out_buf = heap_caps_aligned_calloc(cache_align,
                                                        1,
                                                        s_camera.ppa_out_size,
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    if (s_camera.ppa_out_buf == NULL) {
        esp_vision_camera_release_ppa();
        return ESP_ERR_NO_MEM;
    }

    esp_vision_camera_update_active_window();
    return ESP_OK;
}

static esp_err_t esp_vision_camera_reinit_ppa(void)
{
    esp_vision_camera_release_ppa();
    return esp_vision_camera_init_ppa();
}

static esp_err_t esp_vision_camera_init_buffers(void)
{
    struct v4l2_requestbuffers req = {
        .count = ESP_VISION_CAMERA_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = ESP_VISION_CAMERA_MEMORY_TYPE,
    };

    if (ioctl(s_camera.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "failed to request camera buffers");
        return ESP_FAIL;
    }
    if ((req.count == 0) || (req.count > ESP_VISION_CAMERA_BUFFER_COUNT)) {
        ESP_LOGE(TAG, "camera returned invalid buffer count");
        return ESP_ERR_NO_MEM;
    }
    s_camera.buffer_count = req.count;

    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = ESP_VISION_CAMERA_MEMORY_TYPE,
            .index = i,
        };

        if (ioctl(s_camera.fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "failed to query camera buffer %" PRIu32, i);
            return ESP_FAIL;
        }

        s_camera.buffers[i].ptr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                       s_camera.fd, buf.m.offset);
        s_camera.buffers[i].len = buf.length;
        if ((s_camera.buffers[i].ptr == NULL) || (s_camera.buffers[i].ptr == MAP_FAILED)) {
            s_camera.buffers[i].ptr = NULL;
            s_camera.buffers[i].len = 0;
            ESP_LOGE(TAG, "failed to map camera buffer %" PRIu32, i);
            return ESP_FAIL;
        }

        if (esp_vision_camera_queue_index(i) != ESP_OK) {
            ESP_LOGE(TAG, "failed to queue camera buffer %" PRIu32, i);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static esp_err_t esp_vision_camera_start_stream(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(s_camera.fd, VIDIOC_STREAMON, &type) != 0) {
        return ESP_FAIL;
    }

    s_camera.streaming = true;
    s_camera.dqbuf_restart_count = 0;
    return ESP_OK;
}

static esp_err_t esp_vision_camera_restart_stream(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (s_camera.streaming) {
        if (ioctl(s_camera.fd, VIDIOC_STREAMOFF, &type) != 0) {
            return ESP_FAIL;
        }
        s_camera.streaming = false;
    }

    esp_err_t ret = esp_vision_camera_queue_all_buffers();
    if (ret != ESP_OK) {
        return ret;
    }

    if (ioctl(s_camera.fd, VIDIOC_STREAMON, &type) != 0) {
        return ESP_FAIL;
    }

    s_camera.streaming = true;
    return ESP_OK;
}

static esp_err_t esp_vision_camera_dequeue_buffer(struct v4l2_buffer *buf)
{
    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(buf, 0, sizeof(*buf));
    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = ESP_VISION_CAMERA_MEMORY_TYPE;

    if (ioctl(s_camera.fd, VIDIOC_DQBUF, buf) != 0) {
        return ESP_FAIL;
    }
    if ((buf->index >= ESP_VISION_CAMERA_BUFFER_COUNT) ||
            (s_camera.buffers[buf->index].ptr == NULL) ||
            (buf->flags & V4L2_BUF_FLAG_ERROR)) {
        if (buf->index < ESP_VISION_CAMERA_BUFFER_COUNT) {
            ioctl(s_camera.fd, VIDIOC_QBUF, buf);
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t esp_vision_camera_queue_buffer(struct v4l2_buffer *buf)
{
    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return (ioctl(s_camera.fd, VIDIOC_QBUF, buf) == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t esp_vision_camera_prime_stream(void)
{
    for (int attempt = 0; attempt < ESP_VISION_CAMERA_STREAM_PRIME_ATTEMPTS; attempt++) {
        struct v4l2_buffer buf;
        esp_err_t ret = esp_vision_camera_dequeue_buffer(&buf);
        if (ret == ESP_OK) {
            ret = esp_vision_camera_queue_buffer(&buf);
            if (ret != ESP_OK) {
                return ret;
            }
            s_camera.dqbuf_restart_count = 0;
            return ESP_OK;
        }

        if ((attempt + 1) >= ESP_VISION_CAMERA_STREAM_PRIME_ATTEMPTS) {
            return ret;
        }

        esp_vision_debug_printf("[esp-vision] camera init: dq_restart %d/%d ret=%d\r\n",
                                attempt + 1,
                                ESP_VISION_CAMERA_STREAM_PRIME_ATTEMPTS - 1,
                                (int)ret);
        ret = esp_vision_camera_restart_stream();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_FAIL;
}

static void esp_vision_camera_cleanup(void)
{
    if (s_camera.streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_camera.fd, VIDIOC_STREAMOFF, &type);
        s_camera.streaming = false;
    }

    esp_vision_camera_release_buffers();
    esp_vision_camera_release_ppa();
    s_camera.buffer_count = 0;
    s_camera.dqbuf_restart_count = 0;

    if (s_camera.fd >= 0) {
        close(s_camera.fd);
        s_camera.fd = -1;
    }

    esp_vision_camera_video_deinit();
    s_camera.initialized = false;
}

static esp_err_t esp_vision_camera_init_once(const char **failed_stage)
{
    esp_err_t ret;
    const char *stage = "start";

    stage = "video_init";
    ret = esp_vision_camera_video_init();
    if (ret != ESP_OK) {
        goto fail;
    }

    stage = "open_device";
    ret = esp_vision_camera_open_device();
    if (ret != ESP_OK) {
        goto fail;
    }

    stage = "set_dqbuf_timeout";
    ret = esp_vision_camera_set_dqbuf_timeout();
    if (ret != ESP_OK) {
        goto fail;
    }

    stage = "set_input_format";
    ret = esp_vision_camera_set_input_format();
    if (ret != ESP_OK) {
        goto fail;
    }

    stage = "init_ppa";
    ret = esp_vision_camera_init_ppa();
    if (ret != ESP_OK) {
        goto fail;
    }

    stage = "init_buffers";
    ret = esp_vision_camera_init_buffers();
    if (ret != ESP_OK) {
        goto fail;
    }

    stage = "start_stream";
    ret = esp_vision_camera_start_stream();
    if (ret != ESP_OK) {
        goto fail;
    }

    stage = "prime_stream";
    ret = esp_vision_camera_prime_stream();
    if (ret != ESP_OK) {
        goto fail;
    }

    s_camera.initialized = true;
    esp_vision_debug_printf("[esp-vision] camera started: sensor=0x%04" PRIx32
                            " raw=%" PRIu32 "x%" PRIu32
                            " active=%" PRIu32 "x%" PRIu32 "+%" PRIu32 "+%" PRIu32
                            " output=%" PRIu32 "x%" PRIu32
                            " input=%s driver=esp-video-dvp pixfmt=%" PRIu32 "\r\n",
                            s_camera.sensor_id,
                            s_camera.raw_input_width,
                            s_camera.raw_input_height,
                            s_camera.active_input_width,
                            s_camera.active_input_height,
                            s_camera.active_input_offset_x,
                            s_camera.active_input_offset_y,
                            s_camera.width,
                            s_camera.height,
                            esp_vision_camera_input_name(),
                            (uint32_t)s_camera.output_pixfmt);
    if (failed_stage != NULL) {
        *failed_stage = NULL;
    }
    return ESP_OK;

fail:
    if (failed_stage != NULL) {
        *failed_stage = stage;
    }
    return ret;
}

esp_err_t esp_vision_camera_init(void)
{
    esp_err_t ret = ESP_OK;
    const char *stage = "start";

    if (s_camera.initialized) {
        return ESP_OK;
    }

    if ((s_camera.width == 0) || (s_camera.height == 0) || (s_camera.output_pixfmt == PIXFORMAT_INVALID)) {
        esp_vision_camera_set_defaults();
    }

    for (int attempt = 0; attempt < ESP_VISION_CAMERA_FULL_INIT_ATTEMPTS; attempt++) {
        ret = esp_vision_camera_init_once(&stage);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        esp_vision_camera_cleanup();
        if ((attempt + 1) < ESP_VISION_CAMERA_FULL_INIT_ATTEMPTS) {
            esp_vision_debug_printf("[esp-vision] camera init retry %d/%d: stage=%s ret=%d\r\n",
                                    attempt + 1,
                                    ESP_VISION_CAMERA_FULL_INIT_ATTEMPTS - 1,
                                    stage,
                                    (int)ret);
            vTaskDelay(pdMS_TO_TICKS(ESP_VISION_CAMERA_INIT_RETRY_DELAY_MS));
        }
    }

    esp_vision_debug_printf("[esp-vision] camera init failed: stage=%s ret=%d attempts=%d\r\n",
                            stage,
                            (int)ret,
                            ESP_VISION_CAMERA_FULL_INIT_ATTEMPTS);
    return ret;
}

void esp_vision_camera_deinit(void)
{
    esp_vision_camera_cleanup();
}

bool esp_vision_camera_is_ready(void)
{
    return s_camera.initialized &&
           s_camera.streaming &&
           (s_camera.ppa_handle != NULL) &&
           (s_camera.ppa_out_buf != NULL) &&
           esp_vision_camera_is_supported_input(s_camera.input_pixfmt) &&
           (s_camera.width != 0) &&
           (s_camera.height != 0);
}

esp_err_t esp_vision_camera_set_pixformat(uint32_t pixfmt)
{
    if ((pixfmt != PIXFORMAT_RGB565) && (pixfmt != PIXFORMAT_GRAYSCALE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_camera.output_pixfmt == pixfmt) {
        return ESP_OK;
    }

    s_camera.output_pixfmt = (pixformat_t)pixfmt;
    if (s_camera.initialized) {
        return esp_vision_camera_reinit_ppa();
    }
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

    if ((s_camera.width == width) && (s_camera.height == height)) {
        return ESP_OK;
    }

    s_camera.width = width;
    s_camera.height = height;
    if (s_camera.initialized) {
        return esp_vision_camera_reinit_ppa();
    }
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
    return ESP_OK;
}

bool esp_vision_camera_get_hmirror(void)
{
    return s_camera.hmirror;
}

esp_err_t esp_vision_camera_set_vflip(bool enable)
{
    s_camera.vflip = enable;
    return ESP_OK;
}

bool esp_vision_camera_get_vflip(void)
{
    return s_camera.vflip;
}

uint32_t esp_vision_camera_get_sensor_id(void)
{
    return s_camera.sensor_id;
}

size_t esp_vision_camera_frame_size(void)
{
    return esp_vision_camera_output_size(s_camera.width, s_camera.height, s_camera.output_pixfmt);
}

esp_err_t esp_vision_camera_capture(uint8_t *pixels, size_t pixels_size)
{
    struct v4l2_buffer buf;
    bool dequeued = false;
    esp_err_t ret;
    size_t expected_size;
    size_t input_size;

    if ((pixels == NULL) || !esp_vision_camera_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    expected_size = esp_vision_camera_frame_size();
    if ((expected_size == 0) || (pixels_size < expected_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = esp_vision_camera_dequeue_buffer(&buf);
    if (ret != ESP_OK) {
        if (s_camera.dqbuf_restart_count < ESP_VISION_CAMERA_DQBUF_RESTART_MAX) {
            s_camera.dqbuf_restart_count++;
            esp_vision_debug_printf("[esp-vision] camera capture: dqbuf timeout, restart stream %" PRIu32 "/%d\r\n",
                                    s_camera.dqbuf_restart_count,
                                    ESP_VISION_CAMERA_DQBUF_RESTART_MAX);
            ret = esp_vision_camera_restart_stream();
            if (ret != ESP_OK) {
                return ret;
            }
            ret = esp_vision_camera_dequeue_buffer(&buf);
            if (ret != ESP_OK) {
                esp_vision_debug_printf("[esp-vision] camera capture: dqbuf retry failed ret=%d\r\n", (int)ret);
                return ret;
            }
        } else {
            return ret;
        }
    }
    dequeued = true;

    input_size = (size_t)s_camera.input_stride * (size_t)s_camera.raw_input_height;
    if ((input_size == 0) || (input_size > s_camera.buffers[buf.index].len)) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    esp_cache_msync(s_camera.buffers[buf.index].ptr,
                    input_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                    ESP_CACHE_MSYNC_FLAG_INVALIDATE);

    ppa_srm_oper_config_t ppa_config = {};
    ppa_config.in.buffer = s_camera.buffers[buf.index].ptr;
    ppa_config.in.pic_w = s_camera.raw_input_width;
    ppa_config.in.pic_h = s_camera.raw_input_height;
    ppa_config.in.block_w = s_camera.active_input_width;
    ppa_config.in.block_h = s_camera.active_input_height;
    ppa_config.in.block_offset_x = s_camera.active_input_offset_x;
    ppa_config.in.block_offset_y = s_camera.active_input_offset_y;
    ppa_config.in.srm_cm = (s_camera.input_pixfmt == V4L2_PIX_FMT_YUYV) ?
                           PPA_SRM_COLOR_MODE_YUV422_YUYV :
                           PPA_SRM_COLOR_MODE_RGB565;
    ppa_config.out.buffer = s_camera.ppa_out_buf;
    ppa_config.out.buffer_size = s_camera.ppa_out_size;
    ppa_config.out.pic_w = s_camera.width;
    ppa_config.out.pic_h = s_camera.height;
    ppa_config.out.block_offset_x = 0;
    ppa_config.out.block_offset_y = 0;
    ppa_config.out.srm_cm = esp_vision_camera_output_color_mode(s_camera.output_pixfmt);
    ppa_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    ppa_config.scale_x = (float)s_camera.width / (float)s_camera.active_input_width;
    ppa_config.scale_y = (float)s_camera.height / (float)s_camera.active_input_height;
    ppa_config.byte_swap = (s_camera.input_pixfmt == V4L2_PIX_FMT_RGB565X);
    ppa_config.mirror_x = s_camera.hmirror;
    ppa_config.mirror_y = s_camera.vflip;
    ppa_config.mode = PPA_TRANS_MODE_BLOCKING;

    esp_cache_msync(s_camera.ppa_out_buf,
                    s_camera.ppa_out_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

    ret = ppa_do_scale_rotate_mirror(s_camera.ppa_handle, &ppa_config);
    if (ret != ESP_OK) {
        goto exit;
    }

    esp_cache_msync(s_camera.ppa_out_buf, s_camera.ppa_out_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(pixels, s_camera.ppa_out_buf, expected_size);

exit:
    if (dequeued) {
        esp_err_t queue_ret = esp_vision_camera_queue_buffer(&buf);
        if ((ret == ESP_OK) && (queue_ret != ESP_OK)) {
            ret = queue_ret;
        }
    }
    return ret;
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
    status->sensor_id = s_camera.sensor_id;
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

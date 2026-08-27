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

#include "driver/ppa.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_manager_includes.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_video_ioctl.h"
#include "hal/usb_serial_jtag_ll.h"
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
#define ESP_VISION_CAMERA_SENSOR_ID_UNKNOWN 0x0000
#define ESP_VISION_CAMERA_OV3640_SENSOR_ID_1 0x364C
#define ESP_VISION_CAMERA_OV3640_SENSOR_ID_2 0x3641
#define ESP_VISION_CAMERA_DQBUF_TIMEOUT_MS 500
#define ESP_VISION_CAMERA_DQBUF_RESTART_MAX 1
#define ESP_VISION_CAMERA_STREAM_PRIME_ATTEMPTS 3
#define ESP_VISION_CAMERA_FULL_INIT_ATTEMPTS 3
#define ESP_VISION_CAMERA_INIT_RETRY_DELAY_MS 100

typedef struct {
    void *ptr;
    size_t len;
} esp_vision_camera_buffer_t;

typedef struct {
    bool initialized;
    bool video_initialized;
    bool board_resources_claimed;
    bool usb_serial_jtag_pad_was_enabled;
    bool usb_serial_jtag_clock_was_enabled;
    bool streaming;
    bool hmirror;
    bool vflip;
    int fd;
    uint32_t sensor_id;
    uint32_t usb_serial_jtag_intr_enable_mask;
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
    const char *device_path;
    esp_vision_camera_buffer_t buffers[ESP_VISION_CAMERA_BUFFER_COUNT];
} esp_vision_camera_context_t;

static const char *TAG = "esp_vision_camera";
static esp_vision_camera_context_t s_camera = {
    .fd = -1,
    .sensor_id = ESP_VISION_CAMERA_SENSOR_ID_UNKNOWN,
    .width = ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_WIDTH,
    .height = ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_HEIGHT,
    .output_pixfmt = PIXFORMAT_RGB565,
};

static void esp_vision_camera_set_defaults(void)
{
    s_camera.fd = -1;
    s_camera.sensor_id = ESP_VISION_CAMERA_SENSOR_ID_UNKNOWN;
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
    case V4L2_PIX_FMT_UYVY:
        return "uyvy";
    default:
        return "unknown";
    }
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

static esp_err_t esp_vision_camera_claim_board_resources(void)
{
    if (s_camera.board_resources_claimed) {
        return ESP_OK;
    }

    s_camera.usb_serial_jtag_pad_was_enabled =
        usb_serial_jtag_ll_phy_is_pad_enabled();
    s_camera.usb_serial_jtag_clock_was_enabled =
        usb_serial_jtag_ll_module_is_enabled();
    s_camera.usb_serial_jtag_intr_enable_mask =
        s_camera.usb_serial_jtag_clock_was_enabled
        ? usb_serial_jtag_ll_get_intr_ena_status()
        : 0;

    /*
     * Camera D2 on GPIO33 shares the built-in USB Serial/JTAG PHY. ESP-VISION
     * uses USB-OTG for EV-MUX, so the built-in PHY can be released while the
     * camera owns the left slot.
     */
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_phy_enable_pad(false);
    usb_serial_jtag_ll_enable_bus_clock(false);
    s_camera.board_resources_claimed = true;

    ESP_LOGI(TAG,
             "Mosaico CameraBoard resources claimed; USB Serial/JTAG pads released for DVP GPIO33");
    return ESP_OK;
}

static void esp_vision_camera_release_board_resources(void)
{
    if (!s_camera.board_resources_claimed) {
        return;
    }

    if (s_camera.usb_serial_jtag_clock_was_enabled) {
        usb_serial_jtag_ll_enable_bus_clock(true);
    }
    usb_serial_jtag_ll_phy_enable_pad(
        s_camera.usb_serial_jtag_pad_was_enabled);
    if (s_camera.usb_serial_jtag_clock_was_enabled &&
            s_camera.usb_serial_jtag_intr_enable_mask != 0) {
        usb_serial_jtag_ll_ena_intr_mask(
            s_camera.usb_serial_jtag_intr_enable_mask);
    }

    s_camera.board_resources_claimed = false;
    ESP_LOGI(TAG, "Mosaico CameraBoard resources released");
}

static esp_err_t esp_vision_camera_video_init(void)
{
    esp_err_t ret = esp_board_device_power_ctrl(ESP_BOARD_DEVICE_NAME_CAMERA, true);
    ESP_RETURN_ON_ERROR(ret, TAG, "board manager failed to enable camera power");

    ret = esp_vision_camera_claim_board_resources();
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to claim Mosaico camera resources");

    vTaskDelay(pdMS_TO_TICKS(ESP_VISION_CAMERA_POWER_STABILIZATION_MS));

    ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_CAMERA);
    ESP_RETURN_ON_ERROR(ret, TAG, "board manager failed to initialize camera");

    dev_camera_handle_t *camera = NULL;
    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_CAMERA,
                                              (void **)&camera);
    if ((ret != ESP_OK) || (camera == NULL) || (camera->dev_path == NULL)) {
        (void)esp_board_manager_deinit_device_by_name(ESP_BOARD_DEVICE_NAME_CAMERA);
        return (ret == ESP_OK) ? ESP_ERR_INVALID_STATE : ret;
    }

    s_camera.device_path = camera->dev_path;
    s_camera.video_initialized = true;
    return ESP_OK;
}

static esp_err_t esp_vision_camera_query_sensor_id(void)
{
    esp_cam_sensor_id_t chip_id = {0};
    struct v4l2_ext_control control = {
        .id = ESP_CAM_SENSOR_IOC_G_CHIP_ID,
        .size = sizeof(chip_id),
        .p_u8 = (uint8_t *) &chip_id,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL,
        .count = 1,
        .controls = &control,
    };

    if (ioctl(s_camera.fd, VIDIOC_G_EXT_CTRLS, &controls) != 0) {
        ESP_LOGE(TAG, "failed to query camera sensor ID");
        return ESP_FAIL;
    }

    if ((chip_id.pid != ESP_VISION_CAMERA_OV3640_SENSOR_ID_1) &&
            (chip_id.pid != ESP_VISION_CAMERA_OV3640_SENSOR_ID_2)) {
        ESP_LOGE(TAG, "unsupported camera sensor ID: 0x%04" PRIx16, chip_id.pid);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_camera.sensor_id = chip_id.pid;
    s_camera.hmirror = false;
    s_camera.vflip = false;
    return ESP_OK;
}

static void esp_vision_camera_video_deinit(void)
{
    if (s_camera.video_initialized) {
        (void)esp_board_manager_deinit_device_by_name(ESP_BOARD_DEVICE_NAME_CAMERA);
        s_camera.device_path = NULL;
        s_camera.video_initialized = false;
    }
    esp_vision_camera_release_board_resources();
}

static esp_err_t esp_vision_camera_open_device(void)
{
    struct v4l2_capability capability = {0};

    if (s_camera.device_path == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_camera.fd = open(s_camera.device_path, O_RDWR);
    if (s_camera.fd < 0) {
        ESP_LOGE(TAG, "failed to open %s", s_camera.device_path);
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
    return esp_vision_camera_is_rgb565_input(pixfmt) ||
           (pixfmt == V4L2_PIX_FMT_YUYV) ||
           (pixfmt == V4L2_PIX_FMT_UYVY);
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
    if (format.fmt.pix.pixelformat != pixfmt) {
        ESP_LOGE(TAG, "camera selected unexpected input format: requested=0x%08" PRIx32
                 " actual=0x%08" PRIx32,
                 pixfmt,
                 format.fmt.pix.pixelformat);
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
    if ((s_camera.sensor_id != ESP_VISION_CAMERA_OV3640_SENSOR_ID_1) &&
            (s_camera.sensor_id != ESP_VISION_CAMERA_OV3640_SENSOR_ID_2)) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_vision_camera_configure_input_format(ESP_VISION_CAMERA_RAW_INPUT_WIDTH,
                                                    ESP_VISION_CAMERA_RAW_INPUT_HEIGHT,
                                                    V4L2_PIX_FMT_UYVY);
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
    s_camera.sensor_id = ESP_VISION_CAMERA_SENSOR_ID_UNKNOWN;
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

    stage = "query_sensor_id";
    ret = esp_vision_camera_query_sensor_id();
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
    ppa_config.in.srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY;
    ppa_config.out.buffer = s_camera.ppa_out_buf;
    ppa_config.out.buffer_size = s_camera.ppa_out_size;
    ppa_config.out.pic_w = s_camera.width;
    ppa_config.out.pic_h = s_camera.height;
    ppa_config.out.block_offset_x = 0;
    ppa_config.out.block_offset_y = 0;
    ppa_config.out.srm_cm = esp_vision_camera_output_color_mode(s_camera.output_pixfmt);
    ppa_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    ppa_config.scale_x = (float)s_camera.height / (float)s_camera.active_input_width;
    ppa_config.scale_y = (float)s_camera.width / (float)s_camera.active_input_height;
    ppa_config.byte_swap = false;
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

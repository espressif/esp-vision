/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_BOARD_CONFIG_H
#define ESP_VISION_BOARD_CONFIG_H

#define ESP_VISION_BOARD_ARCH                  "ESP32P4"
#define ESP_VISION_BOARD_TYPE                  "ESP32_P4X_VISION"
#define ESP_VISION_PORT_ESP32                       (1)

#define ESP_VISION_IMLIB_PROFILER_ENABLE            (0)
#define ESP_VISION_IMLIB_GPU_ENABLE                 (0)
#define ESP_VISION_IMLIB_JPEG_CODEC_ENABLE          (0)

#define ESP_VISION_CACHE_LINE_SIZE                  (32)
#define ESP_VISION_ALLOC_ALIGNMENT                  (ESP_VISION_CACHE_LINE_SIZE)
#define ESP_VISION_DMA_ALIGNMENT                    (ESP_VISION_CACHE_LINE_SIZE)

#define ESP_VISION_JPEG_QUALITY_LOW                 (60)
#define ESP_VISION_JPEG_QUALITY_HIGH                (60)
#define ESP_VISION_JPEG_QUALITY_THRESHOLD           (320 * 240 * 2)

/* Camera configuration. */
#define ESP_VISION_CAMERA_SENSOR_ID                 (0xda4a)
#define ESP_VISION_CAMERA_RAW_INPUT_WIDTH           (1280)
#define ESP_VISION_CAMERA_RAW_INPUT_HEIGHT          (720)
#define ESP_VISION_CAMERA_ACTIVE_INPUT_WIDTH        (640)
#define ESP_VISION_CAMERA_ACTIVE_INPUT_HEIGHT       (480)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QQVGA_WIDTH    (160)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QQVGA_HEIGHT   (120)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_WIDTH     (320)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_HEIGHT    (240)
#define ESP_VISION_CAMERA_PPA_OUTPUT_VGA_WIDTH      (640)
#define ESP_VISION_CAMERA_PPA_OUTPUT_VGA_HEIGHT     (480)
#define ESP_VISION_CAMERA_BUFFER_COUNT              (3)
#define ESP_VISION_CAMERA_SCCB_I2C_PORT             (0)
#define ESP_VISION_CAMERA_SCCB_I2C_SCL_PIN          (31)
#define ESP_VISION_CAMERA_SCCB_I2C_SDA_PIN          (32)
#define ESP_VISION_CAMERA_SENSOR_RESET_PIN          (17)
#define ESP_VISION_CAMERA_SENSOR_PWDN_PIN           (19)
#define ESP_VISION_CAMERA_SCCB_I2C_FREQ             (100000)
#define ESP_VISION_CAMERA_XCLK_PIN                  (22)
#define ESP_VISION_CAMERA_XCLK_FREQ                 (20000000)
#define ESP_VISION_CAMERA_DVP_PCLK_PIN              (15)
#define ESP_VISION_CAMERA_DVP_VSYNC_PIN             (18)
#define ESP_VISION_CAMERA_DVP_HSYNC_PIN             (20)
#define ESP_VISION_CAMERA_DVP_D0_PIN                (13)
#define ESP_VISION_CAMERA_DVP_D1_PIN                (27)
#define ESP_VISION_CAMERA_DVP_D2_PIN                (26)
#define ESP_VISION_CAMERA_DVP_D3_PIN                (28)
#define ESP_VISION_CAMERA_DVP_D4_PIN                (14)
#define ESP_VISION_CAMERA_DVP_D5_PIN                (16)
#define ESP_VISION_CAMERA_DVP_D6_PIN                (23)
#define ESP_VISION_CAMERA_DVP_D7_PIN                (21)

/* SD card configuration. */
#define ESP_VISION_SDCARD_MOUNT_PATH                "/sdcard"
#define ESP_VISION_SDCARD_SLOT                      (0)
#define ESP_VISION_SDCARD_BUS_WIDTH                 (4)

#endif /* ESP_VISION_BOARD_CONFIG_H */

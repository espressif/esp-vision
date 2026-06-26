/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_BOARD_CONFIG_H
#define ESP_VISION_BOARD_CONFIG_H

#define ESP_VISION_BOARD_ARCH                  "ESP32P4"
#define ESP_VISION_BOARD_TYPE                  "ESP32_P4X_FUNCTION_EV_BOARD"
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
#define ESP_VISION_CAMERA_SENSOR_ID                 (0x2336)
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
#define ESP_VISION_CAMERA_BUFFER_COUNT              (4)
#define ESP_VISION_CAMERA_SCCB_I2C_PORT             (1)
#define ESP_VISION_CAMERA_SCCB_I2C_SCL_PIN          (8)
#define ESP_VISION_CAMERA_SCCB_I2C_SDA_PIN          (7)
#define ESP_VISION_CAMERA_SENSOR_RESET_PIN          (-1)
#define ESP_VISION_CAMERA_SENSOR_PWDN_PIN           (-1)
#define ESP_VISION_CAMERA_SCCB_I2C_FREQ             (400000)

/* Display configuration. */
#define ESP_VISION_LCD_WIDTH                        (1024)
#define ESP_VISION_LCD_HEIGHT                       (600)
#define ESP_VISION_LCD_BPP                          (16)
#define ESP_VISION_LCD_PIN_RST                      (27)
#define ESP_VISION_LCD_PIN_BL                       (26)
#define ESP_VISION_LCD_BACKLIGHT_CH                 (1)
#define ESP_VISION_LCD_BACKLIGHT_TIMER              (1)
#define ESP_VISION_LCD_BACKLIGHT_PWM_HZ             (5000)
#define ESP_VISION_LCD_MIPI_DSI_BUS_ID              (0)
#define ESP_VISION_LCD_MIPI_DSI_LANE_NUM            (2)
#define ESP_VISION_LCD_MIPI_DSI_LANE_BITRATE_MBPS   (1000)
#define ESP_VISION_LCD_MIPI_DSI_PHY_LDO_CHAN_ID     (3)
#define ESP_VISION_LCD_MIPI_DSI_PHY_LDO_VOLTAGE_MV  (2500)

/* SD card configuration. */
#define ESP_VISION_SDCARD_MOUNT_PATH                "/sdcard"
#define ESP_VISION_SDCARD_SLOT                      (0)
#define ESP_VISION_SDCARD_BUS_WIDTH                 (4)
#define ESP_VISION_SDCARD_LDO_CHAN_ID               (4)

#endif /* ESP_VISION_BOARD_CONFIG_H */

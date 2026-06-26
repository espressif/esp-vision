/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_BOARD_CONFIG_H
#define ESP_VISION_BOARD_CONFIG_H

#define ESP_VISION_BOARD_ARCH                  "ESP32P4"
#define ESP_VISION_BOARD_TYPE                  "ESP32_P4X_EYE"
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
#define ESP_VISION_CAMERA_SENSOR_ID                 (0x2710)
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
#define ESP_VISION_CAMERA_BUFFER_COUNT              (2)
#define ESP_VISION_CAMERA_SCCB_I2C_PORT             (0)
#define ESP_VISION_CAMERA_SCCB_I2C_SCL_PIN          (13)
#define ESP_VISION_CAMERA_SCCB_I2C_SDA_PIN          (14)
#define ESP_VISION_CAMERA_SENSOR_RESET_PIN          (26)
#define ESP_VISION_CAMERA_SENSOR_PWDN_PIN           (12)
#define ESP_VISION_CAMERA_XCLK_PIN                  (11)
#define ESP_VISION_CAMERA_SCCB_I2C_FREQ             (100000)
#define ESP_VISION_CAMERA_XCLK_FREQ                 (24000000)

/* LCD configuration. */
#define ESP_VISION_LCD_WIDTH                        (240)
#define ESP_VISION_LCD_HEIGHT                       (240)
#define ESP_VISION_LCD_SPI_HOST                     (2)
#define ESP_VISION_LCD_PIXEL_CLOCK_HZ               (80 * 1000 * 1000)
#define ESP_VISION_LCD_CMD_BITS                     (8)
#define ESP_VISION_LCD_PARAM_BITS                   (8)
#define ESP_VISION_LCD_BPP                          (16)
#define ESP_VISION_LCD_PIN_MOSI                     (16)
#define ESP_VISION_LCD_PIN_CLK                      (17)
#define ESP_VISION_LCD_PIN_CS                       (18)
#define ESP_VISION_LCD_PIN_DC                       (19)
#define ESP_VISION_LCD_PIN_RST                      (15)
#define ESP_VISION_LCD_PIN_BL                       (20)
#define ESP_VISION_LCD_BACKLIGHT_CH                 (0)
#define ESP_VISION_LCD_BACKLIGHT_TIMER              (0)
#define ESP_VISION_LCD_BACKLIGHT_PWM_HZ             (5000)
#define ESP_VISION_LCD_BACKLIGHT_OUTPUT_INVERT      (1)

/* SD card configuration. */
#define ESP_VISION_SDCARD_MOUNT_PATH                "/sdcard"
#define ESP_VISION_SDCARD_SLOT                      (0)
#define ESP_VISION_SDCARD_BUS_WIDTH                 (4)
#define ESP_VISION_SDCARD_EN_PIN                    (46)
#define ESP_VISION_SDCARD_EN_ACTIVE_LEVEL           (0)
#define ESP_VISION_SDCARD_DETECT_PIN                (45)
#define ESP_VISION_SDCARD_DETECT_PRESENT_LEVEL      (0)
#define ESP_VISION_SDCARD_LDO_CHAN_ID               (4)

#endif /* ESP_VISION_BOARD_CONFIG_H */

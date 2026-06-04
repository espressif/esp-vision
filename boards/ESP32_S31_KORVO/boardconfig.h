/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_BOARD_CONFIG_H
#define ESP_VISION_BOARD_CONFIG_H

#define ESP_VISION_BOARD_ARCH                  "ESP32S31"
#define ESP_VISION_BOARD_TYPE                  "ESP32_S31_KORVO"
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
#define ESP_VISION_CAMERA_SENSOR_ID                 (0x3660)
#define ESP_VISION_CAMERA_RAW_INPUT_WIDTH           (640)
#define ESP_VISION_CAMERA_RAW_INPUT_HEIGHT          (480)
#define ESP_VISION_CAMERA_ACTIVE_INPUT_WIDTH        (640)
#define ESP_VISION_CAMERA_ACTIVE_INPUT_HEIGHT       (480)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QQVGA_WIDTH    (160)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QQVGA_HEIGHT   (120)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_WIDTH     (320)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_HEIGHT    (240)
#define ESP_VISION_CAMERA_BUFFER_COUNT              (3)
#define ESP_VISION_CAMERA_SCCB_I2C_PORT             (0)
#define ESP_VISION_CAMERA_SCCB_I2C_SCL_PIN          (1)
#define ESP_VISION_CAMERA_SCCB_I2C_SDA_PIN          (0)
#define ESP_VISION_CAMERA_SENSOR_RESET_PIN          (-1)
#define ESP_VISION_CAMERA_SENSOR_PWDN_PIN           (-1)
#define ESP_VISION_CAMERA_SCCB_I2C_FREQ             (100000)
#define ESP_VISION_CAMERA_XCLK_PIN                  (55)
#define ESP_VISION_CAMERA_XCLK_FREQ                 (20000000)
#define ESP_VISION_CAMERA_DVP_PCLK_PIN              (54)
#define ESP_VISION_CAMERA_DVP_VSYNC_PIN             (56)
#define ESP_VISION_CAMERA_DVP_HSYNC_PIN             (57)
#define ESP_VISION_CAMERA_DVP_D0_PIN                (46)
#define ESP_VISION_CAMERA_DVP_D1_PIN                (47)
#define ESP_VISION_CAMERA_DVP_D2_PIN                (48)
#define ESP_VISION_CAMERA_DVP_D3_PIN                (49)
#define ESP_VISION_CAMERA_DVP_D4_PIN                (50)
#define ESP_VISION_CAMERA_DVP_D5_PIN                (51)
#define ESP_VISION_CAMERA_DVP_D6_PIN                (52)
#define ESP_VISION_CAMERA_DVP_D7_PIN                (53)

/* Display configuration. */
#define ESP_VISION_LCD_WIDTH                        (800)
#define ESP_VISION_LCD_HEIGHT                       (480)
#define ESP_VISION_LCD_BPP                          (16)
#define ESP_VISION_LCD_PIXEL_CLOCK_HZ               (20 * 1000 * 1000)
#define ESP_VISION_LCD_DATA_WIDTH                   (16)
#define ESP_VISION_LCD_HSYNC_PULSE_WIDTH            (1)
#define ESP_VISION_LCD_HSYNC_BACK_PORCH             (40)
#define ESP_VISION_LCD_HSYNC_FRONT_PORCH            (20)
#define ESP_VISION_LCD_VSYNC_PULSE_WIDTH            (1)
#define ESP_VISION_LCD_VSYNC_BACK_PORCH             (10)
#define ESP_VISION_LCD_VSYNC_FRONT_PORCH            (5)
#define ESP_VISION_LCD_PIN_D0                       (8)
#define ESP_VISION_LCD_PIN_D1                       (9)
#define ESP_VISION_LCD_PIN_D2                       (10)
#define ESP_VISION_LCD_PIN_D3                       (11)
#define ESP_VISION_LCD_PIN_D4                       (12)
#define ESP_VISION_LCD_PIN_D5                       (13)
#define ESP_VISION_LCD_PIN_D6                       (14)
#define ESP_VISION_LCD_PIN_D7                       (15)
#define ESP_VISION_LCD_PIN_D8                       (16)
#define ESP_VISION_LCD_PIN_D9                       (17)
#define ESP_VISION_LCD_PIN_D10                      (18)
#define ESP_VISION_LCD_PIN_D11                      (19)
#define ESP_VISION_LCD_PIN_D12                      (33)
#define ESP_VISION_LCD_PIN_D13                      (34)
#define ESP_VISION_LCD_PIN_D14                      (35)
#define ESP_VISION_LCD_PIN_D15                      (36)
#define ESP_VISION_LCD_PIN_PCLK                     (40)
#define ESP_VISION_LCD_PIN_DE                       (43)
#define ESP_VISION_LCD_PIN_HSYNC                    (44)
#define ESP_VISION_LCD_PIN_VSYNC                    (45)
#define ESP_VISION_LCD_PIN_DISP_EN                  (-1)
#define ESP_VISION_LCD_PIN_BL                       (-1)

/* SD card configuration. */
#define ESP_VISION_SDCARD_MOUNT_PATH                "/sdcard"
#define ESP_VISION_SDCARD_SLOT                      (0)
#define ESP_VISION_SDCARD_BUS_WIDTH                 (4)
#define ESP_VISION_SDCARD_CLK_PIN                   (24)
#define ESP_VISION_SDCARD_CMD_PIN                   (25)
#define ESP_VISION_SDCARD_D0_PIN                    (20)
#define ESP_VISION_SDCARD_D1_PIN                    (21)
#define ESP_VISION_SDCARD_D2_PIN                    (22)
#define ESP_VISION_SDCARD_D3_PIN                    (23)
#define ESP_VISION_SDCARD_CTRL_PIN                  (39)
#define ESP_VISION_SDCARD_CTRL_ACTIVE_LEVEL         (0)

#endif /* ESP_VISION_BOARD_CONFIG_H */

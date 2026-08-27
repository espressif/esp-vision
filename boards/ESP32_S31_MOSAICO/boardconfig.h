/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_BOARD_CONFIG_H
#define ESP_VISION_BOARD_CONFIG_H

#define ESP_VISION_BOARD_ARCH                       "ESP32S31"
#define ESP_VISION_BOARD_TYPE                       "ESP32_S31_MOSAICO"
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

/* OV3640 DVP camera configuration. */
#define ESP_VISION_CAMERA_RAW_INPUT_WIDTH           (640)
#define ESP_VISION_CAMERA_RAW_INPUT_HEIGHT          (480)
#define ESP_VISION_CAMERA_ACTIVE_INPUT_WIDTH        (640)
#define ESP_VISION_CAMERA_ACTIVE_INPUT_HEIGHT       (480)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QQVGA_WIDTH    (120)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QQVGA_HEIGHT   (160)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_WIDTH     (240)
#define ESP_VISION_CAMERA_PPA_OUTPUT_QVGA_HEIGHT    (320)
#define ESP_VISION_CAMERA_PPA_OUTPUT_VGA_WIDTH      (480)
#define ESP_VISION_CAMERA_PPA_OUTPUT_VGA_HEIGHT     (640)
#define ESP_VISION_CAMERA_BUFFER_COUNT              (3)
#define ESP_VISION_CAMERA_POWER_STABILIZATION_MS    (20)
#define ESP_VISION_CAMERA_SCCB_I2C_PORT             (0)
#define ESP_VISION_CAMERA_SCCB_I2C_SCL_PIN          (1)
#define ESP_VISION_CAMERA_SCCB_I2C_SDA_PIN          (0)
#define ESP_VISION_CAMERA_SENSOR_RESET_PIN          (53)
#define ESP_VISION_CAMERA_SENSOR_PWDN_PIN           (48)
#define ESP_VISION_CAMERA_SCCB_I2C_FREQ             (400000)
#define ESP_VISION_CAMERA_XCLK_PIN                  (-1)
#define ESP_VISION_CAMERA_XCLK_FREQ                 (24000000)
#define ESP_VISION_CAMERA_DVP_PCLK_PIN              (17)
#define ESP_VISION_CAMERA_DVP_VSYNC_PIN             (55)
#define ESP_VISION_CAMERA_DVP_HSYNC_PIN             (19)
#define ESP_VISION_CAMERA_DVP_D0_PIN                (16)
#define ESP_VISION_CAMERA_DVP_D1_PIN                (15)
#define ESP_VISION_CAMERA_DVP_D2_PIN                (33)
#define ESP_VISION_CAMERA_DVP_D3_PIN                (4)
#define ESP_VISION_CAMERA_DVP_D4_PIN                (14)
#define ESP_VISION_CAMERA_DVP_D5_PIN                (12)
#define ESP_VISION_CAMERA_DVP_D6_PIN                (18)
#define ESP_VISION_CAMERA_DVP_D7_PIN                (13)

/* CO5300 QSPI display configuration. */
#define ESP_VISION_LCD_WIDTH                        (480)
#define ESP_VISION_LCD_HEIGHT                       (480)
#define ESP_VISION_LCD_BPP                          (16)
#define ESP_VISION_LCD_PIXEL_CLOCK_HZ               (40 * 1000 * 1000)
#define ESP_VISION_LCD_SPI_HOST                     (1)
#define ESP_VISION_LCD_PIN_D0                       (36)
#define ESP_VISION_LCD_PIN_D1                       (51)
#define ESP_VISION_LCD_PIN_D2                       (35)
#define ESP_VISION_LCD_PIN_D3                       (9)
#define ESP_VISION_LCD_PIN_CLK                      (42)
#define ESP_VISION_LCD_PIN_CS                       (50)
#define ESP_VISION_LCD_PIN_RST                      (44)
#define ESP_VISION_LCD_PIN_POWER                    (60)

#endif /* ESP_VISION_BOARD_CONFIG_H */

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_BOARD_CONFIG_H
#define ESP_VISION_BOARD_CONFIG_H

/* Copy this directory to boards/<BOARD> and replace every TEMPLATE value. */

#define ESP_VISION_BOARD_ARCH                  "TEMPLATE"
#define ESP_VISION_BOARD_TYPE                  "TEMPLATE"
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

/* EV-MUX v4 physical defaults and background Wi-Fi/TCP policy.
 * Native/Hosted WLAN is enabled separately in the port/IDF configuration.
 * Factory credentials apply only when no saved NVS profile exists.
 */
#define ESP_VISION_ENABLE_BACKGROUND_NETWORK       (1)
#define ESP_VISION_WIFI_DEFAULT_SSID                ""
#define ESP_VISION_WIFI_DEFAULT_PASSWORD            ""
#define ESP_VISION_TRANSPORT_DEFAULT_SINK          "usj"
#define ESP_VISION_TRANSPORT_FALLBACK_SINK         "usj"
#define ESP_VISION_ENABLE_TCP_TRANSPORT            (0)
#define ESP_VISION_TCP_PORT                        (3232)

#endif /* ESP_VISION_BOARD_CONFIG_H */

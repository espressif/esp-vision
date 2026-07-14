# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

if(NOT DEFINED ESP_VISION_IDF_OVERLAY)
    set(ESP_VISION_IDF_OVERLAY "unknown")
endif()

if(NOT ESP_VISION_IDF_OVERLAY STREQUAL "release6.1")
    message(FATAL_ERROR
        "ESP32_S31_KORVO is supported only with ESP-IDF release/v6.1. "
        "Current overlay: ${ESP_VISION_IDF_OVERLAY}. "
        "Source an ESP-IDF release/v6.1 environment before building this board."
    )
endif()

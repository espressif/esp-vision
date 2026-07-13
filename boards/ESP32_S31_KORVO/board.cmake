# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

if(NOT DEFINED ESP_VISION_IDF_OVERLAY)
    set(ESP_VISION_IDF_OVERLAY "unknown")
endif()

if((NOT ESP_VISION_IDF_OVERLAY STREQUAL "release6.1")
        AND (NOT ESP_VISION_IDF_OVERLAY STREQUAL "master"))
    message(FATAL_ERROR
        "ESP32_S31_KORVO is currently supported only with the ESP-VISION "
        "IDF release6.1 or master overlay. Current overlay: ${ESP_VISION_IDF_OVERLAY}. "
        "Source an ESP-IDF release/v6.1 or master environment before building this board."
    )
endif()

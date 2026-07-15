/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_FIRMWARE_INFO_H
#define ESP_VISION_FIRMWARE_INFO_H

// Stable machine-readable identity. Keep this independent of user-facing
// branding so host tools can reliably distinguish ESP-VISION firmware.
#define ESP_VISION_FIRMWARE_ID "esp-vision"

// CMake injects ESP-IDF's PROJECT_VER into every ESP-VISION source. Release
// builds therefore use the release tag (for example 2026.07.16), while
// development builds retain the git-describe suffix. Never silently publish
// an unknown version to host tools.
#ifndef ESP_VISION_FIRMWARE_VERSION
#error "ESP_VISION_FIRMWARE_VERSION must be provided by the firmware build"
#endif

#endif // ESP_VISION_FIRMWARE_INFO_H

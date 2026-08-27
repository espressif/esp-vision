# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

set(ESP_VISION_ENABLE_BARCODE ON)
set(ESP_VISION_USE_BOARD_MANAGER ON)
set(ESP_VISION_BOARD_MANAGER_ESP_BOARDS_SETUP "esp32_p4_function_ev_board/setup_device.c")
set(ESP_VISION_BOARD_MANAGER_EXTRA_COMPONENTS espressif__esp_lcd_ek79007)

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/*
 * This is the display hook skeleton for boards that do not use ESP Board
 * Manager. Remove this file when enabling ESP_VISION_USE_BOARD_MANAGER for a
 * board, because platform/display.c provides these hooks in that configuration.
 */

esp_err_t esp_vision_board_display_backlight_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void esp_vision_board_display_set_backlight(uint32_t backlight)
{
    (void)backlight;
}

esp_err_t esp_vision_board_display_init_panel(uint32_t width,
                                              uint32_t height,
                                              esp_lcd_panel_io_handle_t *io_handle,
                                              esp_lcd_panel_handle_t *panel_handle)
{
    (void)width;
    (void)height;

    if (io_handle != NULL) {
        *io_handle = NULL;
    }
    if (panel_handle != NULL) {
        *panel_handle = NULL;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

void esp_vision_board_display_deinit_panel(esp_lcd_panel_io_handle_t io_handle,
                                           esp_lcd_panel_handle_t panel_handle)
{
    (void)io_handle;
    (void)panel_handle;
}

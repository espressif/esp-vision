/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "ESP32_S31_MOSAICO_SETUP";
static esp_err_t (*s_co5300_draw_bitmap)(esp_lcd_panel_t *panel,
                                         int x_start,
                                         int y_start,
                                         int x_end,
                                         int y_end,
                                         const void *color_data);

static esp_err_t esp_vision_mosaico_draw_bitmap(esp_lcd_panel_t *panel,
                                                int x_start,
                                                int y_start,
                                                int x_end,
                                                int y_end,
                                                const void *color_data)
{
    uint16_t *pixels = (uint16_t *)color_data;
    size_t count = (size_t)(x_end - x_start) * (size_t)(y_end - y_start);

    for (size_t i = 0; i < count; i++) {
        pixels[i] = (uint16_t)((pixels[i] >> 8) | (pixels[i] << 8));
    }
    return s_co5300_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_data);
}

static const co5300_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0x11, NULL, 0, 60},
    {0xfe, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1c, (uint8_t[]){0xa0}, 1, 0},
    {0xfe, (uint8_t[]){0x00}, 1, 0},
    {0xc4, (uint8_t[]){0x80}, 1, 0},
    {0x3a, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xff}, 1, 0},
    {0x63, (uint8_t[]){0xff}, 1, 0},
    {0x2a, (uint8_t[]){0x00, 0x00, 0x01, 0xdf}, 4, 0},
    {0x2b, (uint8_t[]){0x00, 0x00, 0x01, 0xdf}, 4, 0},
    {0x36, (uint8_t[]){0xa0}, 1, 0},
};

static const co5300_vendor_config_t s_lcd_vendor_config = {
    .init_cmds = s_lcd_init_cmds,
    .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
    .flags = {
        .use_qspi_interface = 1,
    },
};

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_config,
                                    esp_lcd_panel_handle_t *panel_handle)
{
    ESP_RETURN_ON_FALSE((io != NULL) && (panel_config != NULL) &&
                        (panel_handle != NULL), ESP_ERR_INVALID_ARG, TAG,
                        "invalid CO5300 factory arguments");

    esp_lcd_panel_dev_config_t config = {0};
    memcpy(&config, panel_config, sizeof(config));
    config.vendor_config = (void *)&s_lcd_vendor_config;

    esp_err_t ret = esp_lcd_new_panel_co5300(io, &config, panel_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "create CO5300 panel failed");

    s_co5300_draw_bitmap = (*panel_handle)->draw_bitmap;
    (*panel_handle)->draw_bitmap = esp_vision_mosaico_draw_bitmap;

    return esp_lcd_panel_set_gap(*panel_handle, 0, 0);
}

esp_err_t lcd_panel_set_brightness_entry_t(esp_lcd_panel_handle_t panel,
                                           uint8_t percent)
{
    return esp_lcd_panel_co5300_set_brightness(panel, percent);
}

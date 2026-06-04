/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "soc/soc_caps.h"

#include "boardconfig.h"

#if SOC_LCDCAM_RGB_LCD_SUPPORTED

static bool s_backlight_inited;

void esp_vision_board_display_deinit_panel(esp_lcd_panel_io_handle_t io_handle,
                                           esp_lcd_panel_handle_t panel_handle);

static void esp_vision_board_display_fill_data_pins(gpio_num_t data_gpio_nums[ESP_LCD_RGB_BUS_WIDTH_MAX])
{
    const gpio_num_t pins[ESP_LCD_RGB_BUS_WIDTH_MAX] = {
        ESP_VISION_LCD_PIN_D0,
        ESP_VISION_LCD_PIN_D1,
        ESP_VISION_LCD_PIN_D2,
        ESP_VISION_LCD_PIN_D3,
        ESP_VISION_LCD_PIN_D4,
        ESP_VISION_LCD_PIN_D5,
        ESP_VISION_LCD_PIN_D6,
        ESP_VISION_LCD_PIN_D7,
        ESP_VISION_LCD_PIN_D8,
        ESP_VISION_LCD_PIN_D9,
        ESP_VISION_LCD_PIN_D10,
        ESP_VISION_LCD_PIN_D11,
        ESP_VISION_LCD_PIN_D12,
        ESP_VISION_LCD_PIN_D13,
        ESP_VISION_LCD_PIN_D14,
        ESP_VISION_LCD_PIN_D15,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
    };

    memcpy(data_gpio_nums, pins, sizeof(pins));
}

static esp_err_t esp_vision_board_display_config_output_gpio(gpio_num_t gpio_num, int level)
{
    if (gpio_num == GPIO_NUM_NC) {
        return ESP_OK;
    }

    const gpio_config_t config = {
        .pin_bit_mask = BIT64(gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }
    return gpio_set_level(gpio_num, level);
}

esp_err_t esp_vision_board_display_backlight_init(void)
{
    if (s_backlight_inited) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vision_board_display_config_output_gpio(ESP_VISION_LCD_PIN_BL, 0);
    if (ret == ESP_OK) {
        s_backlight_inited = true;
    }
    return ret;
}

void esp_vision_board_display_set_backlight(uint32_t backlight)
{
    (void)esp_vision_board_display_config_output_gpio(ESP_VISION_LCD_PIN_BL, backlight > 0);
}

esp_err_t esp_vision_board_display_init_panel(uint32_t width,
                                              uint32_t height,
                                              esp_lcd_panel_io_handle_t *io_handle,
                                              esp_lcd_panel_handle_t *panel_handle)
{
    if ((width != ESP_VISION_LCD_WIDTH) || (height != ESP_VISION_LCD_HEIGHT) ||
            (io_handle == NULL) || (panel_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *io_handle = NULL;
    *panel_handle = NULL;

    esp_err_t ret = esp_vision_board_display_config_output_gpio(ESP_VISION_LCD_PIN_BL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = ESP_VISION_LCD_PIXEL_CLOCK_HZ,
            .h_res = ESP_VISION_LCD_WIDTH,
            .v_res = ESP_VISION_LCD_HEIGHT,
            .hsync_pulse_width = ESP_VISION_LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = ESP_VISION_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = ESP_VISION_LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = ESP_VISION_LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = ESP_VISION_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = ESP_VISION_LCD_VSYNC_FRONT_PORCH,
            .flags.pclk_active_neg = true,
        },
        .data_width = ESP_VISION_LCD_DATA_WIDTH,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = 0,
        .dma_burst_size = 64,
        .hsync_gpio_num = ESP_VISION_LCD_PIN_HSYNC,
        .vsync_gpio_num = ESP_VISION_LCD_PIN_VSYNC,
        .de_gpio_num = ESP_VISION_LCD_PIN_DE,
        .pclk_gpio_num = ESP_VISION_LCD_PIN_PCLK,
        .disp_gpio_num = ESP_VISION_LCD_PIN_DISP_EN,
        .flags.fb_in_psram = true,
    };
    esp_vision_board_display_fill_data_pins(panel_config.data_gpio_nums);

    ret = esp_lcd_new_rgb_panel(&panel_config, panel_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_lcd_panel_reset(*panel_handle);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_init(*panel_handle);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_disp_on_off(*panel_handle, true);
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            ret = ESP_OK;
        }
    }
    if (ret == ESP_OK) {
        esp_vision_board_display_set_backlight(100);
    } else {
        esp_vision_board_display_deinit_panel(NULL, *panel_handle);
        *panel_handle = NULL;
    }

    return ret;
}

void esp_vision_board_display_deinit_panel(esp_lcd_panel_io_handle_t io_handle,
                                           esp_lcd_panel_handle_t panel_handle)
{
    (void)io_handle;

    if (panel_handle != NULL) {
        esp_lcd_panel_disp_on_off(panel_handle, false);
        esp_lcd_panel_del(panel_handle);
    }
    esp_vision_board_display_set_backlight(0);
}

#else

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

#endif

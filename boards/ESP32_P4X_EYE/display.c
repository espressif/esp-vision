/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "boardconfig.h"

static bool s_backlight_inited;

void esp_vision_board_display_deinit_panel(esp_lcd_panel_io_handle_t io_handle,
                                           esp_lcd_panel_handle_t panel_handle);

esp_err_t esp_vision_board_display_backlight_init(void)
{
    if (s_backlight_inited) {
        return ESP_OK;
    }

    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = (ledc_timer_t)ESP_VISION_LCD_BACKLIGHT_TIMER,
        .freq_hz = ESP_VISION_LCD_BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    esp_err_t ret = ledc_timer_config(&timer_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ledc_channel_config_t channel_config = {
        .gpio_num = ESP_VISION_LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)ESP_VISION_LCD_BACKLIGHT_CH,
        .timer_sel = (ledc_timer_t)ESP_VISION_LCD_BACKLIGHT_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = ESP_VISION_LCD_BACKLIGHT_OUTPUT_INVERT,
    };

    ret = ledc_channel_config(&channel_config);
    if (ret != ESP_OK) {
        return ret;
    }

    s_backlight_inited = true;
    return ESP_OK;
}

void esp_vision_board_display_set_backlight(uint32_t backlight)
{
    if (backlight > 100U) {
        backlight = 100U;
    }

    uint32_t duty = (1023U * backlight) / 100U;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ESP_VISION_LCD_BACKLIGHT_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ESP_VISION_LCD_BACKLIGHT_CH);
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

    spi_host_device_t spi_host = (spi_host_device_t)ESP_VISION_LCD_SPI_HOST;
    spi_bus_config_t bus_config = {
        .sclk_io_num = ESP_VISION_LCD_PIN_CLK,
        .mosi_io_num = ESP_VISION_LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = ESP_VISION_LCD_WIDTH * 40 * (ESP_VISION_LCD_BPP / 8),
    };

    esp_err_t ret = spi_bus_initialize(spi_host, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = ESP_VISION_LCD_PIN_DC,
        .cs_gpio_num = ESP_VISION_LCD_PIN_CS,
        .pclk_hz = ESP_VISION_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = ESP_VISION_LCD_CMD_BITS,
        .lcd_param_bits = ESP_VISION_LCD_PARAM_BITS,
        .spi_mode = 3,
        .trans_queue_depth = 2,
    };

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_config, io_handle);
    if (ret != ESP_OK) {
        spi_bus_free(spi_host);
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = ESP_VISION_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = ESP_VISION_LCD_BPP,
    };

    ret = esp_lcd_new_panel_st7789(*io_handle, &panel_config, panel_handle);
    if (ret != ESP_OK) {
        esp_lcd_panel_io_del(*io_handle);
        *io_handle = NULL;
        spi_bus_free(spi_host);
        return ret;
    }

    ret = esp_lcd_panel_reset(*panel_handle);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_init(*panel_handle);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_invert_color(*panel_handle, true);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_disp_on_off(*panel_handle, true);
    }
    if (ret != ESP_OK) {
        esp_vision_board_display_deinit_panel(*io_handle, *panel_handle);
        *io_handle = NULL;
        *panel_handle = NULL;
    }

    return ret;
}

void esp_vision_board_display_deinit_panel(esp_lcd_panel_io_handle_t io_handle,
                                           esp_lcd_panel_handle_t panel_handle)
{
    if (panel_handle != NULL) {
        esp_lcd_panel_disp_on_off(panel_handle, false);
        esp_lcd_panel_del(panel_handle);
    }
    if (io_handle != NULL) {
        esp_lcd_panel_io_del(io_handle);
    }
    spi_bus_free((spi_host_device_t)ESP_VISION_LCD_SPI_HOST);
}

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#include "boardconfig.h"

#if SOC_MIPI_DSI_SUPPORTED

#define EK79007_PAD_CONTROL           (0xB2)
#define EK79007_DSI_2_LANE            (0x10)
#define EK79007_DSI_4_LANE            (0x00)
#define EK79007_CMD_SHLR_BIT          (1ULL << 0)
#define EK79007_CMD_UPDN_BIT          (1ULL << 1)
#define EK79007_MDCTL_VALUE_DEFAULT   (0x01)

#define EK79007_PANEL_BUS_DSI_2CH_CONFIG()              \
    {                                                   \
        .bus_id = 0,                                    \
        .num_data_lanes = 2,                            \
        .phy_clk_src = 0,                               \
        .lane_bit_rate_mbps = 900,                      \
    }

#define EK79007_PANEL_IO_DBI_CONFIG()   \
    {                                   \
        .virtual_channel = 0,           \
        .lcd_cmd_bits = 8,              \
        .lcd_param_bits = 8,            \
    }

#define EK79007_1024_600_PANEL_60HZ_CONFIG(color_format) \
    {                                                    \
        .virtual_channel = 0,                            \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,     \
        .dpi_clock_freq_mhz = 52,                        \
        .in_color_format = color_format,                 \
        .num_fbs = 1,                                    \
        .video_timing = {                                \
            .h_size = 1024,                              \
            .v_size = 600,                               \
            .hsync_pulse_width = 10,                     \
            .hsync_back_porch = 160,                     \
            .hsync_front_porch = 160,                    \
            .vsync_pulse_width = 1,                      \
            .vsync_back_porch = 23,                      \
            .vsync_front_porch = 12,                     \
        },                                               \
    }

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} ek79007_lcd_init_cmd_t;

typedef struct {
    const ek79007_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        esp_lcd_dsi_bus_handle_t dsi_bus;
        const esp_lcd_dpi_panel_config_t *dpi_config;
        uint8_t lane_num;
    } mipi_config;
} ek79007_vendor_config_t;

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val;
    const ek79007_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    uint8_t lane_num;
    struct {
        unsigned int reset_level: 1;
    } flags;
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} ek79007_panel_t;

static const char *TAG = "esp_vision_lcd";
static bool s_backlight_ready;
static esp_ldo_channel_handle_t s_dsi_phy_ldo;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;

void esp_vision_board_display_deinit_panel(esp_lcd_panel_io_handle_t io_handle,
                                           esp_lcd_panel_handle_t panel_handle);

static const ek79007_lcd_init_cmd_t vendor_specific_init_default[] = {
    {0x80, (uint8_t[]) {0x8B}, 1, 0},
    {0x81, (uint8_t[]) {0x78}, 1, 0},
    {0x82, (uint8_t[]) {0x84}, 1, 0},
    {0x83, (uint8_t[]) {0x88}, 1, 0},
    {0x84, (uint8_t[]) {0xA8}, 1, 0},
    {0x85, (uint8_t[]) {0xE3}, 1, 0},
    {0x86, (uint8_t[]) {0x88}, 1, 0},
    {0x11, (uint8_t[]) {0x00}, 0, 120},
};

static esp_err_t panel_ek79007_send_init_cmds(ek79007_panel_t *ek79007)
{
    esp_lcd_panel_io_handle_t io = ek79007->io;
    const ek79007_lcd_init_cmd_t *init_cmds = vendor_specific_init_default;
    uint16_t init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(vendor_specific_init_default[0]);
    uint8_t lane_command = EK79007_DSI_2_LANE;

    switch (ek79007->lane_num) {
    case 0:
    case 2:
        lane_command = EK79007_DSI_2_LANE;
        break;
    case 4:
        lane_command = EK79007_DSI_4_LANE;
        break;
    default:
        ESP_LOGE(TAG, "invalid EK79007 lane number: %u", ek79007->lane_num);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, EK79007_PAD_CONTROL, (uint8_t[]) {
        lane_command,
    }, 1), TAG, "send EK79007 lane config failed");

    if (ek79007->init_cmds != NULL) {
        init_cmds = ek79007->init_cmds;
        init_cmds_size = ek79007->init_cmds_size;
    }

    for (int i = 0; i < init_cmds_size; i++) {
        if ((init_cmds[i].cmd == LCD_CMD_MADCTL) && (init_cmds[i].data_bytes > 0)) {
            ek79007->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
        }

        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io,
                                                      init_cmds[i].cmd,
                                                      init_cmds[i].data,
                                                      init_cmds[i].data_bytes),
                            TAG,
                            "send EK79007 init command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }

    return ESP_OK;
}

static esp_err_t panel_ek79007_del(esp_lcd_panel_t *panel)
{
    ek79007_panel_t *ek79007 = (ek79007_panel_t *)panel->user_data;

    ESP_RETURN_ON_ERROR(ek79007->del(panel), TAG, "delete EK79007 DPI panel failed");
    if (ek79007->reset_gpio_num >= 0) {
        gpio_reset_pin(ek79007->reset_gpio_num);
    }
    free(ek79007);
    return ESP_OK;
}

static esp_err_t panel_ek79007_init(esp_lcd_panel_t *panel)
{
    ek79007_panel_t *ek79007 = (ek79007_panel_t *)panel->user_data;

    ESP_RETURN_ON_ERROR(panel_ek79007_send_init_cmds(ek79007), TAG, "send EK79007 init commands failed");
    return ek79007->init(panel);
}

static esp_err_t panel_ek79007_reset(esp_lcd_panel_t *panel)
{
    ek79007_panel_t *ek79007 = (ek79007_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = ek79007->io;

    if (ek79007->reset_gpio_num >= 0) {
        gpio_set_level(ek79007->reset_gpio_num, ek79007->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(ek79007->reset_gpio_num, !ek79007->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(20));
    } else if (io != NULL) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send EK79007 reset failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

static esp_err_t panel_ek79007_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    ek79007_panel_t *ek79007 = (ek79007_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = ek79007->io;
    uint8_t madctl_val = ek79007->madctl_val;

    ESP_RETURN_ON_FALSE(io != NULL, ESP_ERR_INVALID_STATE, TAG, "invalid EK79007 panel IO");

    if (mirror_x) {
        madctl_val |= EK79007_CMD_SHLR_BIT;
    } else {
        madctl_val &= ~EK79007_CMD_SHLR_BIT;
    }

    if (mirror_y) {
        madctl_val |= EK79007_CMD_UPDN_BIT;
    } else {
        madctl_val &= ~EK79007_CMD_UPDN_BIT;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        madctl_val,
    }, 1), TAG, "send EK79007 mirror command failed");

    ek79007->madctl_val = madctl_val;
    return ESP_OK;
}

static esp_err_t panel_ek79007_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    ek79007_panel_t *ek79007 = (ek79007_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = ek79007->io;
    uint8_t command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;

    ESP_RETURN_ON_FALSE(io != NULL, ESP_ERR_INVALID_STATE, TAG, "invalid EK79007 panel IO");
    return esp_lcd_panel_io_tx_param(io, command, NULL, 0);
}

static esp_err_t panel_ek79007_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    ek79007_panel_t *ek79007 = (ek79007_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = ek79007->io;
    uint8_t command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;

    ESP_RETURN_ON_FALSE(io != NULL, ESP_ERR_INVALID_STATE, TAG, "invalid EK79007 panel IO");
    return esp_lcd_panel_io_tx_param(io, command, NULL, 0);
}

static esp_err_t esp_vision_lcd_new_panel_ek79007(const esp_lcd_panel_io_handle_t io,
                                                  const esp_lcd_panel_dev_config_t *panel_dev_config,
                                                  esp_lcd_panel_handle_t *ret_panel)
{
    ek79007_vendor_config_t *vendor_config = NULL;
    ek79007_panel_t *ek79007 = NULL;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE((io != NULL) && (panel_dev_config != NULL) && (ret_panel != NULL),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid EK79007 arguments");

    vendor_config = (ek79007_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE((vendor_config != NULL) &&
                        (vendor_config->mipi_config.dpi_config != NULL) &&
                        (vendor_config->mipi_config.dsi_bus != NULL),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid EK79007 vendor config");

    ek79007 = (ek79007_panel_t *)calloc(1, sizeof(ek79007_panel_t));
    ESP_RETURN_ON_FALSE(ek79007 != NULL, ESP_ERR_NO_MEM, TAG, "no memory for EK79007 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        io_conf.mode = GPIO_MODE_OUTPUT;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure EK79007 reset GPIO failed");
    }

    ek79007->io = io;
    ek79007->init_cmds = vendor_config->init_cmds;
    ek79007->init_cmds_size = vendor_config->init_cmds_size;
    ek79007->lane_num = vendor_config->mipi_config.lane_num;
    ek79007->reset_gpio_num = panel_dev_config->reset_gpio_num;
    ek79007->flags.reset_level = panel_dev_config->flags.reset_active_high;
    ek79007->madctl_val = EK79007_MDCTL_VALUE_DEFAULT;

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus,
                                            vendor_config->mipi_config.dpi_config,
                                            ret_panel),
                      err,
                      TAG,
                      "create EK79007 DPI panel failed");

    ek79007->del = (*ret_panel)->del;
    ek79007->init = (*ret_panel)->init;
    (*ret_panel)->del = panel_ek79007_del;
    (*ret_panel)->init = panel_ek79007_init;
    (*ret_panel)->reset = panel_ek79007_reset;
    (*ret_panel)->mirror = panel_ek79007_mirror;
    (*ret_panel)->invert_color = panel_ek79007_invert_color;
    (*ret_panel)->disp_on_off = panel_ek79007_disp_on_off;
    (*ret_panel)->user_data = ek79007;

    return ESP_OK;

err:
    if (ek79007 != NULL) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(ek79007);
    }
    return ret;
}

static esp_err_t esp_vision_board_display_backlight_deinit(void)
{
    if (!s_backlight_ready) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = (ledc_timer_t)ESP_VISION_LCD_BACKLIGHT_TIMER,
        .deconfigure = 1,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_pause(LEDC_LOW_SPEED_MODE, (ledc_timer_t)ESP_VISION_LCD_BACKLIGHT_TIMER),
                        TAG,
                        "pause LCD backlight timer failed");
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "deconfigure LCD backlight timer failed");
    s_backlight_ready = false;
    return ESP_OK;
}

esp_err_t esp_vision_board_display_backlight_init(void)
{
    if (s_backlight_ready) {
        return ESP_OK;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num = ESP_VISION_LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)ESP_VISION_LCD_BACKLIGHT_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)ESP_VISION_LCD_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = (ledc_timer_t)ESP_VISION_LCD_BACKLIGHT_TIMER,
        .freq_hz = ESP_VISION_LCD_BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "configure LCD backlight timer failed");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), TAG, "configure LCD backlight channel failed");

    s_backlight_ready = true;
    return ESP_OK;
}

static esp_err_t esp_vision_board_display_brightness_set(int brightness_percent)
{
    if (!s_backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness_percent);
    uint32_t duty_cycle = (1023U * (uint32_t)brightness_percent) / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ESP_VISION_LCD_BACKLIGHT_CH, duty_cycle),
                        TAG,
                        "set LCD backlight duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ESP_VISION_LCD_BACKLIGHT_CH),
                        TAG,
                        "update LCD backlight duty failed");
    return ESP_OK;
}

void esp_vision_board_display_set_backlight(uint32_t backlight)
{
    esp_vision_board_display_brightness_set((int)backlight);
}

esp_err_t esp_vision_board_display_init_panel(uint32_t width,
                                              uint32_t height,
                                              esp_lcd_panel_io_handle_t *io_handle,
                                              esp_lcd_panel_handle_t *panel_handle)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE((io_handle != NULL) && (panel_handle != NULL), ESP_ERR_INVALID_ARG, TAG, "invalid LCD handle");
    ESP_RETURN_ON_FALSE((width == ESP_VISION_LCD_WIDTH) && (height == ESP_VISION_LCD_HEIGHT),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "unsupported LCD resolution");

    *io_handle = NULL;
    *panel_handle = NULL;

    esp_ldo_channel_config_t ldo_config = {
        .chan_id = ESP_VISION_LCD_MIPI_DSI_PHY_LDO_CHAN_ID,
        .voltage_mv = ESP_VISION_LCD_MIPI_DSI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_GOTO_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &s_dsi_phy_ldo), err, TAG, "enable MIPI DSI PHY LDO failed");

    esp_lcd_dsi_bus_config_t bus_config = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    bus_config.bus_id = ESP_VISION_LCD_MIPI_DSI_BUS_ID;
    bus_config.num_data_lanes = ESP_VISION_LCD_MIPI_DSI_LANE_NUM;
    bus_config.lane_bit_rate_mbps = ESP_VISION_LCD_MIPI_DSI_LANE_BITRATE_MBPS;
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus), err, TAG, "create MIPI DSI bus failed");

    esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, io_handle), err, TAG, "create MIPI DBI IO failed");

    esp_lcd_dpi_panel_config_t dpi_config = EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_COLOR_FMT_RGB565);
    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = ESP_VISION_LCD_MIPI_DSI_LANE_NUM,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = ESP_VISION_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = ESP_VISION_LCD_BPP,
        .vendor_config = &vendor_config,
    };

    ESP_GOTO_ON_ERROR(esp_vision_lcd_new_panel_ek79007(*io_handle, &panel_config, panel_handle),
                      err,
                      TAG,
                      "create EK79007 panel failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(*panel_handle), err, TAG, "reset EK79007 panel failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(*panel_handle), err, TAG, "init EK79007 panel failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(*panel_handle, true), err, TAG, "turn on EK79007 panel failed");

    return ESP_OK;

err:
    esp_vision_board_display_deinit_panel(*io_handle, *panel_handle);
    *io_handle = NULL;
    *panel_handle = NULL;
    esp_vision_board_display_backlight_deinit();
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
    if (s_dsi_bus != NULL) {
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
    }
    if (s_dsi_phy_ldo != NULL) {
        esp_ldo_release_channel(s_dsi_phy_ldo);
        s_dsi_phy_ldo = NULL;
    }
    esp_vision_board_display_backlight_deinit();
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

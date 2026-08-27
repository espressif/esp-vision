/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "boardconfig.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"

#define ESP_VISION_MOSAICO_HW_VERSION(major, minor) \
    ((uint16_t)(((uint16_t)(major) << 8) | ((uint16_t)(minor) & 0xffU)))
#define ESP_VISION_MOSAICO_HW_VERSION_MAJOR(version) \
    ((uint8_t)((version) >> 8))
#define ESP_VISION_MOSAICO_HW_VERSION_MINOR(version) \
    ((uint8_t)((version) & 0xffU))
#define ESP_VISION_MOSAICO_HW_VERSION_UNPROGRAMMED \
    ESP_VISION_MOSAICO_HW_VERSION(0, 0)
#define ESP_VISION_MOSAICO_HW_VERSION_V1_0 \
    ESP_VISION_MOSAICO_HW_VERSION(1, 0)
#define ESP_VISION_MOSAICO_HW_VERSION_V1_1 \
    ESP_VISION_MOSAICO_HW_VERSION(1, 1)
#define ESP_VISION_MOSAICO_HW_VERSION_V1_2 \
    ESP_VISION_MOSAICO_HW_VERSION(1, 2)
#define ESP_VISION_MOSAICO_LCD_V1_0_PIN_CLK (44)
#define ESP_VISION_MOSAICO_LCD_V1_0_PIN_RST (42)

typedef enum {
    ESP_VISION_MOSAICO_PIN_MAP_V1_0 = 0,
    ESP_VISION_MOSAICO_PIN_MAP_V1_1_V1_2,
} esp_vision_mosaico_pin_map_t;

static const char *TAG = "MOSAICO_VARIANT";
static esp_vision_mosaico_pin_map_t s_pin_map =
    ESP_VISION_MOSAICO_PIN_MAP_V1_1_V1_2;
static uint16_t s_efuse_version;
static uint8_t s_efuse_version_bytes[2];
static bool s_efuse_unprogrammed;
static bool s_using_fallback;
static bool s_prepared;

extern void boardctrl_startup(void);

static const char *esp_vision_mosaico_pin_map_name(void)
{
    return (s_pin_map == ESP_VISION_MOSAICO_PIN_MAP_V1_0) ?
           "v1.0" : "v1.1/v1.2";
}

static esp_err_t esp_vision_mosaico_apply_lcd_pins(void)
{
    periph_spi_config_t *spi_config = NULL;
    void *display_config_raw = NULL;

    ESP_RETURN_ON_ERROR(
        esp_board_manager_get_periph_config("spi_display",
                                            (void **)&spi_config),
        TAG, "get display SPI configuration failed");
    ESP_RETURN_ON_ERROR(
        esp_board_manager_get_device_config("display_lcd",
                                            &display_config_raw),
        TAG, "get LCD configuration failed");
    ESP_RETURN_ON_FALSE((spi_config != NULL) &&
                        (display_config_raw != NULL),
                        ESP_ERR_INVALID_STATE, TAG,
                        "Mosaico LCD configuration is unavailable");

    int clock_pin = ESP_VISION_LCD_PIN_CLK;
    int reset_pin = ESP_VISION_LCD_PIN_RST;
    if (s_pin_map == ESP_VISION_MOSAICO_PIN_MAP_V1_0) {
        clock_pin = ESP_VISION_MOSAICO_LCD_V1_0_PIN_CLK;
        reset_pin = ESP_VISION_MOSAICO_LCD_V1_0_PIN_RST;
    }

    dev_display_lcd_config_t display_config =
        *(const dev_display_lcd_config_t *)display_config_raw;
    display_config.sub_cfg.spi.panel_config.reset_gpio_num = reset_pin;
    ESP_RETURN_ON_ERROR(
        esp_board_device_override_config("display_lcd", &display_config,
                                         sizeof(display_config)),
        TAG, "override LCD configuration failed");

    spi_config->spi_bus_config.sclk_io_num = clock_pin;
    ESP_LOGI(TAG, "selected %s LCD pin map: CLK=%d RESET=%d",
             esp_vision_mosaico_pin_map_name(), clock_pin, reset_pin);
    return ESP_OK;
}

static esp_err_t esp_vision_mosaico_variant_prepare(void)
{
    if (s_prepared) {
        return ESP_OK;
    }

    s_efuse_version = ESP_VISION_MOSAICO_HW_VERSION_UNPROGRAMMED;
    s_efuse_version_bytes[0] = 0;
    s_efuse_version_bytes[1] = 0;
    esp_err_t ret = esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA,
                                              s_efuse_version_bytes,
                                              sizeof(s_efuse_version_bytes) * 8U);
    if (ret != ESP_OK) {
        s_pin_map = ESP_VISION_MOSAICO_PIN_MAP_V1_1_V1_2;
        s_using_fallback = true;
        ESP_LOGE(TAG, "read hardware version from eFuse failed: %s; "
                 "falling back to the v1.1/v1.2 pin map",
                 esp_err_to_name(ret));
    } else {
        /* espefuse displays the little-endian uint16_t bytes in storage order. */
        s_efuse_version = ESP_VISION_MOSAICO_HW_VERSION(
                              s_efuse_version_bytes[1], s_efuse_version_bytes[0]);
        switch (s_efuse_version) {
        case ESP_VISION_MOSAICO_HW_VERSION_UNPROGRAMMED:
            s_efuse_unprogrammed = true;
            s_pin_map = ESP_VISION_MOSAICO_PIN_MAP_V1_0;
            break;
        case ESP_VISION_MOSAICO_HW_VERSION_V1_0:
            s_pin_map = ESP_VISION_MOSAICO_PIN_MAP_V1_0;
            break;
        case ESP_VISION_MOSAICO_HW_VERSION_V1_1:
        case ESP_VISION_MOSAICO_HW_VERSION_V1_2:
            s_pin_map = ESP_VISION_MOSAICO_PIN_MAP_V1_1_V1_2;
            break;
        default:
            s_pin_map = ESP_VISION_MOSAICO_PIN_MAP_V1_1_V1_2;
            s_using_fallback = true;
            ESP_LOGW(TAG, "unsupported hardware version eFuse bytes "
                     "%02x %02x (normalized=0x%04x); "
                     "falling back to the v1.1/v1.2 pin map",
                     (unsigned int)s_efuse_version_bytes[0],
                     (unsigned int)s_efuse_version_bytes[1],
                     (unsigned int)s_efuse_version);
            break;
        }
    }

    ESP_RETURN_ON_ERROR(esp_vision_mosaico_apply_lcd_pins(), TAG,
                        "apply Mosaico hardware variant failed");
    s_prepared = true;
    if (s_using_fallback) {
        ESP_LOGI(TAG, "hardware version eFuse bytes=%02x %02x (fallback), "
                 "pin map: %s",
                 (unsigned int)s_efuse_version_bytes[0],
                 (unsigned int)s_efuse_version_bytes[1],
                 esp_vision_mosaico_pin_map_name());
    } else if (s_efuse_unprogrammed) {
        ESP_LOGI(TAG, "hardware version: v1.0 (legacy unprogrammed eFuse), "
                 "pin map: %s",
                 esp_vision_mosaico_pin_map_name());
    } else {
        ESP_LOGI(TAG, "hardware version: v%u.%u (eFuse bytes=%02x %02x), "
                 "pin map: %s",
                 (unsigned int)ESP_VISION_MOSAICO_HW_VERSION_MAJOR(
                     s_efuse_version),
                 (unsigned int)ESP_VISION_MOSAICO_HW_VERSION_MINOR(
                     s_efuse_version),
                 (unsigned int)s_efuse_version_bytes[0],
                 (unsigned int)s_efuse_version_bytes[1],
                 esp_vision_mosaico_pin_map_name());
    }
    return ESP_OK;
}

void esp_vision_mosaico_board_startup(void)
{
    boardctrl_startup();

    esp_err_t ret = esp_vision_mosaico_variant_prepare();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hardware variant preparation failed: %s; "
                 "continuing with the generated v1.1/v1.2 configuration",
                 esp_err_to_name(ret));
    }
}

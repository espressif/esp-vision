/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "display.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "boardconfig.h"

#ifdef ESP_VISION_USE_BOARD_MANAGER
#ifdef CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
#include "driver/ledc.h"
#endif
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_manager_includes.h"
#include "esp_log.h"
#endif

#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif

#include "imlib.h"

#ifndef ESP_VISION_LCD_WIDTH
#define ESP_VISION_LCD_WIDTH (0)
#endif

#ifndef ESP_VISION_LCD_HEIGHT
#define ESP_VISION_LCD_HEIGHT (0)
#endif

#ifdef ESP_VISION_USE_BOARD_MANAGER

#ifdef CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
static const char *TAG = "esp_vision_display";
static bool s_board_backlight_initialized;
#endif
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
static bool s_board_display_initialized;
#endif

esp_err_t esp_vision_board_display_backlight_init(void)
{
#ifdef CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
    if (s_board_backlight_initialized) {
        return ESP_OK;
    }
    if (!esp_board_manager_check_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS)) {
        return ESP_OK;
    }

    esp_err_t ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS);
    if (ret == ESP_OK) {
        s_board_backlight_initialized = true;
    }
    return ret;
#else
    return ESP_OK;
#endif
}

void esp_vision_board_display_set_backlight(uint32_t backlight)
{
#ifdef CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
    periph_ledc_handle_t *ledc = NULL;
    dev_ledc_ctrl_config_t *device_config = NULL;
    periph_ledc_config_t *ledc_config = NULL;

    if (!s_board_backlight_initialized ||
            (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS,
                                                 (void **)&ledc) != ESP_OK) ||
            (ledc == NULL) ||
            (esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS,
                                                 (void **)&device_config) != ESP_OK) ||
            (device_config == NULL) ||
            (esp_board_manager_get_periph_config(device_config->ledc_name,
                                                 (void **)&ledc_config) != ESP_OK) ||
            (ledc_config == NULL)) {
        return;
    }

    uint32_t max_duty = (1U << (uint32_t)ledc_config->duty_resolution) - 1U;
    uint32_t duty = (backlight * max_duty) / 100U;
    if ((ledc_set_duty(ledc->speed_mode, ledc->channel, duty) != ESP_OK) ||
            (ledc_update_duty(ledc->speed_mode, ledc->channel) != ESP_OK)) {
        ESP_LOGW(TAG, "failed to set LCD backlight");
    }
#else
    (void)backlight;
#endif
}

esp_err_t esp_vision_board_display_init_panel(uint32_t width,
                                              uint32_t height,
                                              esp_lcd_panel_io_handle_t *io_handle,
                                              esp_lcd_panel_handle_t *panel_handle)
{
    if ((io_handle == NULL) || (panel_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
#ifndef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    (void)width;
    (void)height;
    *io_handle = NULL;
    *panel_handle = NULL;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if ((width != ESP_VISION_LCD_WIDTH) || (height != ESP_VISION_LCD_HEIGHT)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD);
    if (ret != ESP_OK) {
        goto fail;
    }
    s_board_display_initialized = true;

    dev_display_lcd_handles_t *display = NULL;
    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                                              (void **)&display);
    if ((ret != ESP_OK) || (display == NULL) || (display->panel_handle == NULL)) {
        ret = (ret == ESP_OK) ? ESP_ERR_INVALID_STATE : ret;
        goto fail;
    }

    *io_handle = display->io_handle;
    *panel_handle = display->panel_handle;
    return ESP_OK;

fail:
    if (s_board_display_initialized) {
        (void)esp_board_manager_deinit_device_by_name(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD);
        s_board_display_initialized = false;
    }
#ifdef CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
    if (s_board_backlight_initialized) {
        (void)esp_board_manager_deinit_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS);
        s_board_backlight_initialized = false;
    }
#endif
    return ret;
#endif
}

void esp_vision_board_display_deinit_panel(esp_lcd_panel_io_handle_t io_handle,
                                           esp_lcd_panel_handle_t panel_handle)
{
    (void)io_handle;

    if (panel_handle != NULL) {
        (void)esp_lcd_panel_disp_on_off(panel_handle, false);
    }
#ifdef CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
    if (s_board_backlight_initialized) {
        esp_vision_board_display_set_backlight(0);
        (void)esp_board_manager_deinit_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS);
        s_board_backlight_initialized = false;
    }
#endif
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    if (s_board_display_initialized) {
        (void)esp_board_manager_deinit_device_by_name(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD);
        s_board_display_initialized = false;
    }
#endif
}

#else

esp_err_t esp_vision_board_display_init_panel(uint32_t width,
                                              uint32_t height,
                                              esp_lcd_panel_io_handle_t *io_handle,
                                              esp_lcd_panel_handle_t *panel_handle);
void esp_vision_board_display_deinit_panel(esp_lcd_panel_io_handle_t io_handle,
                                           esp_lcd_panel_handle_t panel_handle);
esp_err_t esp_vision_board_display_backlight_init(void);
void esp_vision_board_display_set_backlight(uint32_t backlight);

#endif

typedef struct {
    bool ready;
    uint32_t width;
    uint32_t height;
    uint32_t backlight;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    uint16_t *framebuffer;
    size_t framebuffer_size;
} esp_vision_display_context_t;

static esp_vision_display_context_t s_display;

static uint32_t esp_vision_display_clamp_backlight(uint32_t backlight)
{
    return (backlight > 100U) ? 100U : backlight;
}

static void esp_vision_display_release_framebuffer(void)
{
    if (s_display.framebuffer != NULL) {
        heap_caps_free(s_display.framebuffer);
        s_display.framebuffer = NULL;
    }
    s_display.framebuffer_size = 0;
}

static esp_err_t esp_vision_display_alloc_framebuffer(uint32_t width, uint32_t height)
{
    size_t size = width * height * sizeof(uint16_t);

    s_display.framebuffer = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_display.framebuffer == NULL) {
        s_display.framebuffer = heap_caps_aligned_alloc(16, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    if (s_display.framebuffer == NULL) {
        s_display.framebuffer = heap_caps_aligned_alloc(16, size, MALLOC_CAP_8BIT);
    }
    if (s_display.framebuffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_display.framebuffer_size = size;
    memset(s_display.framebuffer, 0, s_display.framebuffer_size);
    return ESP_OK;
}

static esp_err_t esp_vision_display_panel_disp_on_off(bool on)
{
    esp_err_t ret = esp_lcd_panel_disp_on_off(s_display.panel_handle, on);
    return (ret == ESP_ERR_NOT_SUPPORTED) ? ESP_OK : ret;
}

void esp_vision_display_init0(void)
{
    esp_vision_display_deinit();
    memset(&s_display, 0, sizeof(s_display));
}

uint32_t esp_vision_display_get_default_width(void)
{
    return ESP_VISION_LCD_WIDTH;
}

uint32_t esp_vision_display_get_default_height(void)
{
    return ESP_VISION_LCD_HEIGHT;
}

esp_err_t esp_vision_display_init(uint32_t width, uint32_t height, uint32_t backlight)
{
    if (width == 0) {
        width = ESP_VISION_LCD_WIDTH;
    }
    if (height == 0) {
        height = ESP_VISION_LCD_HEIGHT;
    }
    if ((width == 0) || (height == 0)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_display.ready) {
        if ((s_display.width == width) && (s_display.height == height)) {
            return esp_vision_display_set_backlight(backlight);
        }
        esp_vision_display_deinit();
    }

    esp_err_t ret = esp_vision_display_alloc_framebuffer(width, height);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_vision_board_display_backlight_init();
    if (ret != ESP_OK) {
        esp_vision_display_release_framebuffer();
        return ret;
    }

    ret = esp_vision_board_display_init_panel(width, height, &s_display.io_handle, &s_display.panel_handle);
    if (ret != ESP_OK) {
        esp_vision_display_release_framebuffer();
        return ret;
    }

    s_display.ready = true;
    s_display.width = width;
    s_display.height = height;
    s_display.backlight = esp_vision_display_clamp_backlight(backlight);
    esp_vision_board_display_set_backlight(s_display.backlight);
    return esp_vision_display_clear(false);
}

void esp_vision_display_deinit(void)
{
    if ((s_display.panel_handle != NULL) || (s_display.io_handle != NULL)) {
        esp_vision_board_display_deinit_panel(s_display.io_handle, s_display.panel_handle);
        s_display.panel_handle = NULL;
        s_display.io_handle = NULL;
    }
    esp_vision_display_release_framebuffer();
    s_display.ready = false;
    s_display.width = 0;
    s_display.height = 0;
    s_display.backlight = 0;
}

bool esp_vision_display_is_ready(void)
{
    return s_display.ready;
}

uint32_t esp_vision_display_get_width(void)
{
    return s_display.width;
}

uint32_t esp_vision_display_get_height(void)
{
    return s_display.height;
}

uint32_t esp_vision_display_get_backlight(void)
{
    return s_display.backlight;
}

esp_err_t esp_vision_display_set_backlight(uint32_t backlight)
{
    if (!s_display.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_display.backlight = esp_vision_display_clamp_backlight(backlight);
    esp_vision_board_display_set_backlight(s_display.backlight);
    return ESP_OK;
}

esp_err_t esp_vision_display_clear(bool display_off)
{
    if (!s_display.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (display_off) {
        return esp_vision_display_panel_disp_on_off(false);
    }

    esp_err_t ret = esp_vision_display_panel_disp_on_off(true);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(s_display.framebuffer, 0, s_display.framebuffer_size);
    return esp_lcd_panel_draw_bitmap(s_display.panel_handle,
                                     0,
                                     0,
                                     s_display.width,
                                     s_display.height,
                                     s_display.framebuffer);
}

static void esp_vision_display_get_fit_transform(const image_t *img,
                                                 const rectangle_t *roi,
                                                 int *x,
                                                 int *y,
                                                 float *x_scale,
                                                 float *y_scale)
{
    float scale_x = (float)s_display.width / (float)roi->w;
    float scale_y = (float)s_display.height / (float)roi->h;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    int out_w = (int)((float)roi->w * scale);
    int out_h = (int)((float)roi->h * scale);

    (void)img;

    *x = ((int)s_display.width - out_w) / 2;
    *y = ((int)s_display.height - out_h) / 2;
    *x_scale = scale;
    *y_scale = scale;
}

esp_err_t esp_vision_display_write(const image_t *img, const esp_vision_display_draw_config_t *config)
{
    if (!s_display.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (img == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rectangle_t full_roi = {
        .x = 0,
        .y = 0,
        .w = img->w,
        .h = img->h,
    };
    const rectangle_t *roi = (config != NULL && config->roi != NULL) ? config->roi : &full_roi;
    int x = (config != NULL) ? config->x : 0;
    int y = (config != NULL) ? config->y : 0;
    float x_scale = (config != NULL) ? config->x_scale : 1.0f;
    float y_scale = (config != NULL) ? config->y_scale : 1.0f;

    if ((config == NULL) || config->fit) {
        esp_vision_display_get_fit_transform(img, roi, &x, &y, &x_scale, &y_scale);
    }

    image_t dst_img = {
        .w = (int)s_display.width,
        .h = (int)s_display.height,
        .pixfmt = PIXFORMAT_RGB565,
        .size = s_display.framebuffer_size,
        .data = (uint8_t *)s_display.framebuffer,
    };

    memset(dst_img.data, 0, dst_img.size);
    imlib_draw_image(&dst_img,
                     (image_t *)img,
                     x,
                     y,
                     x_scale,
                     y_scale,
                     (rectangle_t *)roi,
                     -1,
                     255,
                     NULL,
                     NULL,
                     IMAGE_HINT_BLACK_BACKGROUND,
                     NULL,
                     NULL,
                     NULL,
                     NULL);

    return esp_lcd_panel_draw_bitmap(s_display.panel_handle,
                                     0,
                                     0,
                                     s_display.width,
                                     s_display.height,
                                     s_display.framebuffer);
}

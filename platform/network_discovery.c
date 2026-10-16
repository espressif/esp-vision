/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "network_discovery.h"
#include <string.h>
#include "device_identity.h"
#include "boardconfig.h"
#include "firmware_info.h"
#include "mdns.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t s_mutex_buffer;
static SemaphoreHandle_t s_mutex;
static bool s_initialized;
#if ESP_VISION_ENABLE_TCP_TRANSPORT
static bool s_advertised;
static int64_t s_retry_at;
#endif

void esp_vision_discovery_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    }
}

static esp_err_t ensure_locked(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (esp_vision_hostname()[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }
    err = mdns_hostname_set(esp_vision_hostname());
    if (err == ESP_OK) {
        err = mdns_instance_name_set(esp_vision_hostname());
    }
    if (err != ESP_OK) {
        mdns_free();
        return err;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t esp_vision_discovery_ensure(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ensure_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

void esp_vision_discovery_set_listening(bool listening)
{
#if ESP_VISION_ENABLE_TCP_TRANSPORT
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!listening) {
        if (s_advertised) {
            esp_err_t err = mdns_service_remove("_esp-vision", "_tcp");
            if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
                s_advertised = false;
            }
        }
        s_retry_at = 0;
    } else if (!s_advertised && esp_timer_get_time() >= s_retry_at) {
        esp_err_t err = ensure_locked();
        mdns_txt_item_t txt[] = {
            {"txtvers", "1"}, {"id", esp_vision_device_id()},
            {"hostname", esp_vision_hostname()}, {"board", ESP_VISION_BOARD_TYPE},
            {"fw", ESP_VISION_FIRMWARE_VERSION}, {"evmux", "4"},
        };
        size_t txt_size = 0;
        for (size_t i = 0; i < sizeof(txt) / sizeof(txt[0]); ++i) {
            txt_size += 2 + strlen(txt[i].key) + strlen(txt[i].value);
        }
        if (txt_size > 256) {
            err = ESP_ERR_INVALID_SIZE;
        }
        if (err == ESP_OK) {
            err = mdns_service_add(esp_vision_hostname(), "_esp-vision", "_tcp",
                                   ESP_VISION_TCP_PORT, txt, sizeof(txt) / sizeof(txt[0]));
        }
        if (err == ESP_OK) {
            s_advertised = true;
            ESP_LOGI("ev_discovery", "%s.local:%u", esp_vision_hostname(), (unsigned)ESP_VISION_TCP_PORT);
        } else {
            s_retry_at = esp_timer_get_time() + 5000000;
            ESP_LOGW("ev_discovery", "service registration failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_mutex);
#else
    (void)listening;
#endif
}

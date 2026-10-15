/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "network_service.h"
#include "boardconfig.h"
#include "network_service_json.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_system.h"
#include "device_identity.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/version.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#else
#include "mbedtls/sha256.h"
#endif
#include "nvs.h"
#include "py/mpconfig.h"

// Board defaults seed an absent NVS record only; saved user intent wins.

#define NETWORK_NVS_NAMESPACE "ev_network"
#define NETWORK_NVS_KEY "config"
#define NETWORK_RECORD_MAGIC 0x45564e57u
#define NETWORK_RECORD_SCHEMA 1u
#define NETWORK_ACK_TIMEOUT_MS 1000u
#define NETWORK_CONFIG_TIMEOUT_MS 30000u
#define NETWORK_RETRY_MAX_MS 30000u
#define NETWORK_RPC_RESPONSE_MIN_CAPACITY 1024u
#define NETWORK_TASK_STACK_SIZE 4096u
#define NETWORK_TASK_PRIORITY 4u

#define NETWORK_EVENT_WAKE BIT0
#define NETWORK_EVENT_CONNECTED BIT1
#define NETWORK_EVENT_DISCONNECTED BIT2

#if MICROPY_PY_NETWORK_WLAN && \
    ((defined(CONFIG_ESP_WIFI_ENABLED) && CONFIG_ESP_WIFI_ENABLED) || \
     (defined(CONFIG_ESP_WIFI_REMOTE_ENABLED) && CONFIG_ESP_WIFI_REMOTE_ENABLED))
#define ESP_VISION_NETWORK_WIFI_ENABLED 1
#else
#define ESP_VISION_NETWORK_WIFI_ENABLED 0
#endif

static const char *TAG = "ev_network";

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t record_size;
    uint32_t revision;
    uint8_t configured;
    uint8_t enabled;
    uint8_t ssid_length;
    uint8_t password_length;
    char ssid[NETWORK_SSID_MAX];
    char password[NETWORK_PASSWORD_MAX];
    uint32_t checksum;
} network_record_t;

typedef struct {
    char id[NETWORK_OPERATION_ID_LENGTH + 1];
    esp_vision_network_action_t action;
    uint8_t enabled;
    uint8_t ssid_length;
    uint8_t password_length;
    char ssid[NETWORK_SSID_MAX + 1];
    char password[NETWORK_PASSWORD_MAX + 1];
    uint8_t parameter_hash[32];
} network_request_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EventGroupHandle_t s_events;
static TaskHandle_t s_worker;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_netif_initialized;
static bool s_event_loop_ready;
static bool s_wifi_initialized;
static bool s_sta_netif_created;
static bool s_ap_netif_created;
static bool s_wifi_handler_registered;
static bool s_ip_handler_registered;
static bool s_initialized;
static bool s_started;

static network_record_t s_persisted;
static bool s_configuration_error;
static bool s_runtime_override;
static bool s_sta_runtime_controlled;
static bool s_reconnect_paused;
static bool s_python_mutation_active;
static bool s_sta_active;
static bool s_ap_active;
static bool s_sta_connected;
static bool s_sta_associated;
static bool s_sta_connect_requested;
static bool s_connect_in_progress;
static uint8_t s_disconnect_reason;
static uint8_t s_python_reconnect_limit;
static uint8_t s_python_reconnect_count;
static uint8_t s_sta_reconnect_count;
static uint32_t s_retry_count;
static int64_t s_retry_at_us;
static char s_ssid[NETWORK_SSID_MAX + 1];
static char s_ipv4[16];

static bool s_has_operation;
static network_request_t s_active_request;
static esp_vision_network_operation_t s_operation;
static bool s_has_last_operation;
static network_request_t s_last_request;
static esp_vision_network_operation_t s_last_operation;
static bool s_reply_released;
static int64_t s_reply_deadline_us;
static esp_vision_network_changed_cb_t s_changed_callback;
static void *s_changed_context;
static esp_vision_network_changed_cb_t s_ip_ready_callback;
static void *s_ip_ready_context;

static void network_worker(void *arg);

esp_err_t esp_vision_network_netif_init(void)
{
    if (s_netif_initialized) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_init();
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        s_netif_initialized = true;
    }
    return err;
}

static uint32_t retry_delay_seconds(uint32_t count)
{
    if (count == 0) {
        return 1;
    }
    if (count >= 6) {
        return NETWORK_RETRY_MAX_MS / 1000;
    }
    return 1u << (count - 1u);
}

static bool lower_hex_operation_id(const char *id)
{
    if (id == NULL || strlen(id) != NETWORK_OPERATION_ID_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < NETWORK_OPERATION_ID_LENGTH; i++) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool password_valid(const char *password, size_t length)
{
    if (length == 0) {
        return true;
    }
    if (length == NETWORK_PASSWORD_MAX) {
        for (size_t i = 0; i < length; i++) {
            char c = password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        return true;
    }
    if (length < 8 || length > 63) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        uint8_t c = password[i];
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool request_fingerprint(network_request_t *request)
{
    uint8_t data[2 + 1 + NETWORK_SSID_MAX + 1 + NETWORK_PASSWORD_MAX];
    size_t length = 0;
    data[length++] = (uint8_t)request->action;
    data[length++] = request->enabled;
    data[length++] = request->ssid_length;
    memcpy(data + length, request->ssid, request->ssid_length);
    length += request->ssid_length;
    data[length++] = request->password_length;
    memcpy(data + length, request->password, request->password_length);
    length += request->password_length;
#if MBEDTLS_VERSION_MAJOR >= 4
    size_t hash_length = 0;
    bool success = (psa_crypto_init() == PSA_SUCCESS) &&
                   (psa_hash_compute(PSA_ALG_SHA_256, data, length,
                                     request->parameter_hash, sizeof(request->parameter_hash),
                                     &hash_length) == PSA_SUCCESS) &&
                   (hash_length == sizeof(request->parameter_hash));
#else
    bool success = mbedtls_sha256(data, length, request->parameter_hash, 0) == 0;
#endif
    memset(data, 0, sizeof(data));
    return success;
}

static const char *state_name(esp_vision_network_state_t state)
{
    switch (state) {
    case ESP_VISION_NETWORK_UNSUPPORTED: return "unsupported";
    case ESP_VISION_NETWORK_UNCONFIGURED: return "unconfigured";
    case ESP_VISION_NETWORK_DISABLED: return "disabled";
    case ESP_VISION_NETWORK_CONNECTING: return "connecting";
    case ESP_VISION_NETWORK_ONLINE: return "online";
    case ESP_VISION_NETWORK_RETRY_WAIT: return "retry_wait";
    default: return "unconfigured";
    }
}

static const char *operation_state_name(esp_vision_network_operation_state_t state)
{
    switch (state) {
    case ESP_VISION_NETWORK_OPERATION_QUEUED: return "queued";
    case ESP_VISION_NETWORK_OPERATION_RUNNING: return "running";
    case ESP_VISION_NETWORK_OPERATION_SUCCEEDED: return "succeeded";
    case ESP_VISION_NETWORK_OPERATION_FAILED: return "failed";
    default: return "queued";
    }
}

static const char *action_name(esp_vision_network_action_t action)
{
    switch (action) {
    case ESP_VISION_NETWORK_ACTION_CONFIGURE: return "network.configure";
    case ESP_VISION_NETWORK_ACTION_ENABLE: return "network.enable";
    case ESP_VISION_NETWORK_ACTION_FORGET: return "network.forget";
    default: return "network.status";
    }
}

static const char *error_name(esp_vision_network_error_t error)
{
    switch (error) {
    case ESP_VISION_NETWORK_ERROR_AUTH_FAILED: return "AUTH_FAILED";
    case ESP_VISION_NETWORK_ERROR_AP_NOT_FOUND: return "AP_NOT_FOUND";
    case ESP_VISION_NETWORK_ERROR_CONNECT_TIMEOUT: return "CONNECT_TIMEOUT";
    case ESP_VISION_NETWORK_ERROR_IP_TIMEOUT: return "IP_TIMEOUT";
    case ESP_VISION_NETWORK_ERROR_PERSIST_FAILED: return "PERSIST_FAILED";
    case ESP_VISION_NETWORK_ERROR_DRIVER_ERROR: return "DRIVER_ERROR";
    default: return "";
    }
}

static const char *error_message(esp_vision_network_error_t error)
{
    switch (error) {
    case ESP_VISION_NETWORK_ERROR_AUTH_FAILED: return "Authentication failed";
    case ESP_VISION_NETWORK_ERROR_AP_NOT_FOUND: return "Access point not found";
    case ESP_VISION_NETWORK_ERROR_CONNECT_TIMEOUT: return "Connection timed out";
    case ESP_VISION_NETWORK_ERROR_IP_TIMEOUT: return "IP address timed out";
    case ESP_VISION_NETWORK_ERROR_PERSIST_FAILED: return "Could not persist network configuration";
    case ESP_VISION_NETWORK_ERROR_DRIVER_ERROR: return "Wi-Fi driver operation failed";
    default: return "";
    }
}

static void json_append(char *output, size_t capacity, size_t *length, const char *text)
{
    if (capacity == 0 || *length >= capacity) {
        return;
    }
    size_t available = capacity - *length;
    int written = snprintf(output + *length, available, "%s", text);
    if (written < 0 || (size_t)written >= available) {
        *length = capacity;
        output[capacity - 1] = '\0';
    } else {
        *length += (size_t)written;
    }
}

static void json_append_format(char *output, size_t capacity, size_t *length, const char *format, ...)
{
    if (capacity == 0 || *length >= capacity) {
        return;
    }
    va_list args;
    va_start(args, format);
    size_t available = capacity - *length;
    int written = vsnprintf(output + *length, available, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= available) {
        *length = capacity;
        output[capacity - 1] = '\0';
    } else {
        *length += (size_t)written;
    }
}

static void json_append_string(char *output, size_t capacity, size_t *length, const char *text, size_t text_length)
{
    json_append(output, capacity, length, "\"");
    for (size_t i = 0; i < text_length && *length + 8 < capacity; i++) {
        uint8_t c = text[i];
        if (c == '"' || c == '\\') {
            output[(*length)++] = '\\';
            output[(*length)++] = c;
        } else if (c == '\b') {
            json_append(output, capacity, length, "\\b");
        } else if (c == '\f') {
            json_append(output, capacity, length, "\\f");
        } else if (c == '\n') {
            json_append(output, capacity, length, "\\n");
        } else if (c == '\r') {
            json_append(output, capacity, length, "\\r");
        } else if (c == '\t') {
            json_append(output, capacity, length, "\\t");
        } else if (c < 0x20) {
            json_append_format(output, capacity, length, "\\u%04x", c);
        } else {
            output[(*length)++] = c;
        }
    }
    if (*length + 1 < capacity) {
        output[(*length)++] = '"';
        output[*length] = '\0';
    } else if (capacity != 0) {
        output[capacity - 1] = '\0';
        *length = capacity;
    }
}

static void write_rpc_error(char *output, size_t capacity, const char *code, const char *message)
{
    if (output == NULL || capacity == 0) {
        return;
    }
    size_t length = 0;
    output[0] = '\0';
    json_append(output, capacity, &length, "{\"ok\":false,\"error\":{\"code\":");
    json_append_string(output, capacity, &length, code, strlen(code));
    json_append(output, capacity, &length, ",\"message\":");
    json_append_string(output, capacity, &length, message, strlen(message));
    json_append(output, capacity, &length, "}}");
}

static void write_operation(char *output, size_t capacity, size_t *length,
                            const esp_vision_network_operation_t *operation)
{
    json_append(output, capacity, length, "{\"id\":");
    json_append_string(output, capacity, length, operation->id, strlen(operation->id));
    json_append(output, capacity, length, ",\"action\":");
    const char *action = action_name(operation->action);
    json_append_string(output, capacity, length, action, strlen(action));
    json_append(output, capacity, length, ",\"state\":");
    const char *state = operation_state_name(operation->state);
    json_append_string(output, capacity, length, state, strlen(state));
    json_append(output, capacity, length, ",\"error\":");
    if (operation->error == ESP_VISION_NETWORK_ERROR_NONE) {
        json_append(output, capacity, length, "null}");
    } else {
        const char *error = error_name(operation->error);
        const char *message = error_message(operation->error);
        json_append(output, capacity, length, "{\"code\":");
        json_append_string(output, capacity, length, error, strlen(error));
        json_append(output, capacity, length, ",\"message\":");
        json_append_string(output, capacity, length, message, strlen(message));
        json_append(output, capacity, length, "}}");
    }
}

static esp_vision_network_state_t network_state_locked(void)
{
    if (!s_initialized) {
        return ESP_VISION_NETWORK_UNSUPPORTED;
    }
    if (s_sta_connected && s_ipv4[0] != '\0') {
        return ESP_VISION_NETWORK_ONLINE;
    }
    if (s_connect_in_progress) {
        return ESP_VISION_NETWORK_CONNECTING;
    }
    if (s_retry_at_us > esp_timer_get_time()) {
        return ESP_VISION_NETWORK_RETRY_WAIT;
    }
    // Activating WLAN without requesting a connection is an idle state, not
    // an indefinitely pending connect. Candidate attempts take precedence
    // over the still-unconfigured persistent record while a transaction runs.
    if (!s_persisted.configured && !s_sta_connect_requested) {
        return ESP_VISION_NETWORK_UNCONFIGURED;
    }
    if ((!s_persisted.enabled && !s_sta_connect_requested) ||
            (s_sta_runtime_controlled && !s_sta_connect_requested)) {
        return ESP_VISION_NETWORK_DISABLED;
    }
    return ESP_VISION_NETWORK_CONNECTING;
}

static void snapshot_locked(esp_vision_network_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->supported = s_initialized;
    snapshot->enabled = s_persisted.enabled;
    snapshot->configured = s_persisted.configured;
    snapshot->configuration_error = s_configuration_error;
    snapshot->runtime_override = s_runtime_override;
    snapshot->state = network_state_locked();
    snapshot->config_revision = s_persisted.revision;
    memcpy(snapshot->ssid, s_ssid, sizeof(snapshot->ssid));
    memcpy(snapshot->ipv4, s_ipv4, sizeof(snapshot->ipv4));
    snapshot->has_operation = s_has_operation;
    snapshot->has_last_operation = s_has_last_operation;
    if (s_has_operation) {
        snapshot->operation = s_operation;
    }
    if (s_has_last_operation) {
        snapshot->last_operation = s_last_operation;
    }
    snapshot->sta_active = s_sta_active;
    snapshot->ap_active = s_ap_active;
    snapshot->sta_connected = s_sta_connected;
    snapshot->sta_connect_requested = s_sta_connect_requested;
    snapshot->sta_disconnect_reason = s_disconnect_reason;
    snapshot->sta_reconnect_count = s_sta_reconnect_count;
}

esp_err_t esp_vision_network_snapshot(esp_vision_network_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    snapshot_locked(snapshot);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_netif_t *esp_vision_network_sta_netif(void)
{
    return s_sta_netif;
}

esp_netif_t *esp_vision_network_ap_netif(void)
{
    return s_ap_netif;
}

static void notify_changed(void)
{
    esp_vision_network_changed_cb_t callback;
    void *context;
    esp_vision_network_snapshot_t snapshot;
    portENTER_CRITICAL(&s_lock);
    callback = s_changed_callback;
    context = s_changed_context;
    snapshot_locked(&snapshot);
    portEXIT_CRITICAL(&s_lock);
    if (callback != NULL) {
        callback(&snapshot, context);
    }
}

void esp_vision_network_set_changed_callback(esp_vision_network_changed_cb_t callback, void *context)
{
    portENTER_CRITICAL(&s_lock);
    s_changed_callback = callback;
    s_changed_context = context;
    portEXIT_CRITICAL(&s_lock);
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE);
    }
}

void esp_vision_network_set_ip_ready_callback(esp_vision_network_changed_cb_t callback, void *context)
{
    portENTER_CRITICAL(&s_lock);
    s_ip_ready_callback = callback;
    s_ip_ready_context = context;
    bool connected = s_sta_connected;
    portEXIT_CRITICAL(&s_lock);
    if (connected && callback != NULL) {
        esp_vision_network_snapshot_t snapshot;
        esp_vision_network_snapshot(&snapshot);
        callback(&snapshot, context);
    }
}

static uint32_t record_checksum(const network_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = 0xffffffffu;
    size_t end = offsetof(network_record_t, checksum);
    for (size_t i = 0; i < end; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t) - (int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static bool record_valid(const network_record_t *record)
{
    return record->magic == NETWORK_RECORD_MAGIC && record->schema == NETWORK_RECORD_SCHEMA &&
           record->record_size == sizeof(*record) && record->configured <= 1 && record->enabled <= 1 &&
           record->ssid_length <= NETWORK_SSID_MAX && record->password_length <= NETWORK_PASSWORD_MAX &&
           (!record->configured || record->ssid_length != 0) &&
           record_checksum(record) == record->checksum;
}

static void record_prepare(network_record_t *record)
{
    record->magic = NETWORK_RECORD_MAGIC;
    record->schema = NETWORK_RECORD_SCHEMA;
    record->record_size = sizeof(*record);
    record->checksum = record_checksum(record);
}

static esp_err_t persist_record(const network_record_t *source)
{
    network_record_t record = *source;
    record_prepare(&record);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NETWORK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        memset(&record, 0, sizeof(record));
        return err;
    }
    err = nvs_set_blob(handle, NETWORK_NVS_KEY, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    memset(&record, 0, sizeof(record));
    return err;
}

static void restore_factory_defaults(void)
{
    const char *ssid = ESP_VISION_WIFI_DEFAULT_SSID;
    const char *password = ESP_VISION_WIFI_DEFAULT_PASSWORD;
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    network_record_t record = {0};
    bool invalid = ssid_length > NETWORK_SSID_MAX ||
                   !password_valid(password, password_length);
    if (ssid_length != 0 && !invalid) {
        record.configured = 1;
        record.enabled = 1;
        record.revision = 1;
        record.ssid_length = ssid_length;
        record.password_length = password_length;
        memcpy(record.ssid, ssid, ssid_length);
        memcpy(record.password, password, password_length);
        if (persist_record(&record) != ESP_OK) {
            memset(&record, 0, sizeof(record));
            invalid = true;
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_persisted = record;
    s_configuration_error = invalid;
    memcpy(s_ssid, record.ssid, record.ssid_length);
    s_ssid[record.ssid_length] = '\0';
    portEXIT_CRITICAL(&s_lock);
    if (invalid) {
        ESP_LOGW(TAG, "Factory network configuration is invalid or could not be persisted");
    }
    memset(&record, 0, sizeof(record));
}

static void restore_record_from_nvs(void)
{
    network_record_t record = {0};
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NETWORK_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        restore_factory_defaults();
        return;
    }
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_configuration_error = true;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "Unable to open stored network configuration (0x%x)", err);
        return;
    }
    size_t length = sizeof(record);
    err = nvs_get_blob(handle, NETWORK_NVS_KEY, &record, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        restore_factory_defaults();
        return;
    }
    if (err != ESP_OK || length != sizeof(record) || !record_valid(&record)) {
        portENTER_CRITICAL(&s_lock);
        memset(&s_persisted, 0, sizeof(s_persisted));
        s_configuration_error = true;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "Stored network configuration is invalid; it was left untouched");
        memset(&record, 0, sizeof(record));
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_persisted = record;
    s_configuration_error = false;
    s_runtime_override = false;
    s_sta_runtime_controlled = false;
    s_reconnect_paused = false;
    if (record.configured) {
        memcpy(s_ssid, record.ssid, record.ssid_length);
        s_ssid[record.ssid_length] = '\0';
    } else {
        s_ssid[0] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
    memset(&record, 0, sizeof(record));
}

static void network_wifi_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    bool changed = false;
    if (event_base != WIFI_EVENT) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        s_sta_active = true;
        s_python_reconnect_count = 0;
        changed = true;
        break;
    case WIFI_EVENT_STA_STOP:
        s_sta_active = false;
        s_sta_connected = false;
        s_sta_associated = false;
        s_connect_in_progress = false;
        s_ipv4[0] = '\0';
        changed = true;
        break;
    case WIFI_EVENT_STA_CONNECTED:
        s_sta_associated = true;
        changed = true;
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        if (event_data != NULL) {
            wifi_event_sta_disconnected_t *disconnected = event_data;
            s_disconnect_reason = disconnected->reason;
        }
        s_sta_connected = false;
        s_sta_associated = false;
        s_connect_in_progress = false;
        s_ipv4[0] = '\0';
        if (s_sta_connect_requested || s_persisted.enabled) {
            s_python_reconnect_count++;
            if (s_sta_reconnect_count < UINT8_MAX) {
                s_sta_reconnect_count++;
            }
            s_retry_count++;
            uint32_t delay_seconds = retry_delay_seconds(s_retry_count);
            uint32_t jitter_ms = esp_random() % 501;
            s_retry_at_us = esp_timer_get_time() + (int64_t)delay_seconds * 1000000 +
                            (int64_t)jitter_ms * 1000;
        }
        changed = true;
        break;
    case WIFI_EVENT_AP_START:
        s_ap_active = true;
        changed = true;
        break;
    case WIFI_EVENT_AP_STOP:
        s_ap_active = false;
        changed = true;
        break;
    default:
        break;
    }
    portEXIT_CRITICAL(&s_lock);
    if (changed && s_events != NULL) {
        EventBits_t bits = NETWORK_EVENT_WAKE;
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            bits |= NETWORK_EVENT_DISCONNECTED;
        }
        xEventGroupSetBits(s_events, bits);
    }
}

static void network_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base != IP_EVENT) {
        return;
    }
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        char address[16] = {0};
        if (event != NULL) {
            snprintf(address, sizeof(address), IPSTR, IP2STR(&event->ip_info.ip));
        }
        portENTER_CRITICAL(&s_lock);
        s_sta_connected = true;
        s_sta_associated = true;
        s_connect_in_progress = false;
        s_disconnect_reason = 0;
        s_retry_count = 0;
        s_retry_at_us = 0;
        s_python_reconnect_count = 0;
        s_sta_reconnect_count = 0;
        memcpy(s_ipv4, address, sizeof(s_ipv4));
        portEXIT_CRITICAL(&s_lock);
        if (s_events != NULL) {
            xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE | NETWORK_EVENT_CONNECTED);
        }
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
        portENTER_CRITICAL(&s_lock);
        s_sta_connected = false;
        s_ipv4[0] = '\0';
        portEXIT_CRITICAL(&s_lock);
        if (s_events != NULL) {
            xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE | NETWORK_EVENT_DISCONNECTED);
        }
    }
}

static esp_err_t ensure_event_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t esp_vision_network_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
#if ESP_VISION_NETWORK_WIFI_ENABLED
    esp_err_t err;
    err = esp_vision_network_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_event_loop_ready) {
        err = ensure_event_loop();
        if (err != ESP_OK) {
            return err;
        }
        s_event_loop_ready = true;
    }
    if (!s_wifi_initialized) {
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
#if CONFIG_SPIRAM_IGNORE_NOTFOUND
        if (!esp_psram_is_initialized()) {
            config.cache_tx_buf_num = 0;
            config.feature_caps &= ~CONFIG_FEATURE_CACHE_TX_BUF_BIT;
            config.tx_buf_type = 1;
            config.static_tx_buf_num = 0;
            config.dynamic_tx_buf_num = 32;
        }
#endif
        err = esp_wifi_init(&config);
        if (err == ESP_ERR_WIFI_INIT_STATE) {
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            return err;
        }
        s_wifi_initialized = true;
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!s_sta_netif_created) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_sta_netif_created = true;
    }
    if (esp_vision_hostname()[0] != '\0') {
        err = esp_netif_set_hostname(s_sta_netif, esp_vision_hostname());
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!s_ap_netif_created) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_ap_netif_created = true;
    }
    if (!s_wifi_handler_registered) {
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  network_wifi_event, NULL, &s_wifi_handler);
        if (err != ESP_OK) {
            return err;
        }
        s_wifi_handler_registered = true;
    }
    if (!s_ip_handler_registered) {
        err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                  network_ip_event, NULL, &s_ip_handler);
        if (err != ESP_OK) {
            return err;
        }
        s_ip_handler_registered = true;
    }
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "Wi-Fi support is disabled in this firmware");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static bool mode_has_sta(wifi_mode_t mode)
{
    return mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA;
}

static esp_err_t ensure_sta_mode(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        return err;
    }
    if (!mode_has_sta(mode)) {
        mode = (wifi_mode_t)(mode | WIFI_MODE_STA);
        err = esp_wifi_set_mode(mode);
        if (err != ESP_OK) {
            return err;
        }
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) {
        return err;
    }
    return ESP_OK;
}

static wifi_config_t config_from_record(const network_record_t *record)
{
    wifi_config_t config = {0};
    size_t ssid_length = record->ssid_length < sizeof(config.sta.ssid)
                         ? record->ssid_length : sizeof(config.sta.ssid);
    size_t password_length = record->password_length < sizeof(config.sta.password)
                             ? record->password_length : sizeof(config.sta.password);
    memcpy(config.sta.ssid, record->ssid, ssid_length);
    memcpy(config.sta.password, record->password, password_length);
    return config;
}

static bool wait_for_ip(uint32_t timeout_ms, bool interrupt_for_queued_operation)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        portENTER_CRITICAL(&s_lock);
        bool connected = s_sta_connected && s_ipv4[0] != '\0';
        bool disconnected = !s_connect_in_progress && !s_sta_associated;
        bool interrupted = interrupt_for_queued_operation && s_has_operation &&
                           s_operation.state == ESP_VISION_NETWORK_OPERATION_QUEUED;
        portEXIT_CRITICAL(&s_lock);
        if (connected) {
            return true;
        }
        if (disconnected || interrupted) {
            return false;
        }
        xEventGroupWaitBits(s_events, NETWORK_EVENT_CONNECTED | NETWORK_EVENT_DISCONNECTED,
                            pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
    }
    return false;
}

static esp_vision_network_error_t classify_connect_failure(void)
{
    portENTER_CRITICAL(&s_lock);
    uint8_t reason = s_disconnect_reason;
    bool associated = s_sta_associated;
    portEXIT_CRITICAL(&s_lock);
    if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_CONNECTION_FAIL) {
        return ESP_VISION_NETWORK_ERROR_AUTH_FAILED;
    }
    if (reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD ||
            reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
            reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY) {
        return ESP_VISION_NETWORK_ERROR_AP_NOT_FOUND;
    }
    return associated ? ESP_VISION_NETWORK_ERROR_IP_TIMEOUT : ESP_VISION_NETWORK_ERROR_CONNECT_TIMEOUT;
}

static esp_err_t connect_record(const network_record_t *record, uint32_t timeout_ms,
                                bool interrupt_for_queued_operation,
                                esp_vision_network_error_t *failure)
{
    if (failure != NULL) {
        *failure = ESP_VISION_NETWORK_ERROR_DRIVER_ERROR;
    }
    esp_err_t err = ensure_sta_mode();
    if (err != ESP_OK) {
        return err;
    }
    wifi_config_t config = config_from_record(record);
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof(config));
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    s_sta_connect_requested = true;
    s_connect_in_progress = true;
    s_sta_associated = false;
    s_retry_at_us = 0;
    s_disconnect_reason = 0;
    s_ipv4[0] = '\0';
    memcpy(s_ssid, record->ssid, record->ssid_length);
    s_ssid[record->ssid_length] = '\0';
    portEXIT_CRITICAL(&s_lock);
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        portENTER_CRITICAL(&s_lock);
        s_connect_in_progress = false;
        portEXIT_CRITICAL(&s_lock);
        return err;
    }
    if (wait_for_ip(timeout_ms, interrupt_for_queued_operation)) {
        if (failure != NULL) {
            *failure = ESP_VISION_NETWORK_ERROR_NONE;
        }
        return ESP_OK;
    }
    if (failure != NULL) {
        *failure = classify_connect_failure();
    }
    return ESP_ERR_TIMEOUT;
}

static void notify_operation_change(esp_vision_network_operation_state_t state,
                                    esp_vision_network_error_t error)
{
    portENTER_CRITICAL(&s_lock);
    if (s_has_operation) {
        s_operation.state = state;
        s_operation.error = error;
    }
    portEXIT_CRITICAL(&s_lock);
    notify_changed();
}

static void complete_operation(bool succeeded, esp_vision_network_error_t error)
{
    portENTER_CRITICAL(&s_lock);
    if (s_has_operation) {
        s_operation.state = succeeded ? ESP_VISION_NETWORK_OPERATION_SUCCEEDED :
                            ESP_VISION_NETWORK_OPERATION_FAILED;
        s_operation.error = succeeded ? ESP_VISION_NETWORK_ERROR_NONE : error;
        s_last_operation = s_operation;
        /* Keep only the idempotence key. Do not retain completed credentials. */
        memset(&s_last_request, 0, sizeof(s_last_request));
        memcpy(s_last_request.id, s_active_request.id, sizeof(s_last_request.id));
        s_last_request.action = s_active_request.action;
        memcpy(s_last_request.parameter_hash, s_active_request.parameter_hash,
               sizeof(s_last_request.parameter_hash));
        s_has_last_operation = true;
        s_has_operation = false;
        s_reply_released = false;
        memset(&s_active_request, 0, sizeof(s_active_request));
        s_python_mutation_active = false;
    }
    portEXIT_CRITICAL(&s_lock);
    notify_changed();
}

static void schedule_retry(void)
{
    portENTER_CRITICAL(&s_lock);
    s_connect_in_progress = false;
    s_sta_connected = false;
    s_ipv4[0] = '\0';
    if (s_retry_at_us == 0 || s_retry_at_us <= esp_timer_get_time()) {
        s_retry_count++;
        uint32_t delay_seconds = retry_delay_seconds(s_retry_count);
        uint32_t jitter_ms = esp_random() % 501;
        s_retry_at_us = esp_timer_get_time() + (int64_t)delay_seconds * 1000000 +
                        (int64_t)jitter_ms * 1000;
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool should_reconnect(void)
{
    portENTER_CRITICAL(&s_lock);
    bool requested = s_sta_runtime_controlled ? s_sta_connect_requested :
                     (s_persisted.configured && s_persisted.enabled);
    bool connecting = s_connect_in_progress;
    bool connected = s_sta_connected;
    bool operation = s_has_operation;
    bool mutation = s_python_mutation_active;
    bool limit_reached = s_runtime_override && s_python_reconnect_limit != 0 &&
                         s_python_reconnect_count >= s_python_reconnect_limit;
    bool due = s_retry_at_us == 0 || s_retry_at_us <= esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
    return !operation && !mutation && requested && !connecting && !connected &&
           !limit_reached && due;
}

static void try_reconnect(void)
{
    portENTER_CRITICAL(&s_lock);
    bool requested = s_sta_runtime_controlled ? s_sta_connect_requested :
                     (s_persisted.configured && s_persisted.enabled);
    bool limit_reached = s_runtime_override && s_python_reconnect_limit != 0 &&
                         s_python_reconnect_count >= s_python_reconnect_limit;
    bool due = s_retry_at_us == 0 || s_retry_at_us <= esp_timer_get_time();
    bool attempt = !s_has_operation && !s_python_mutation_active && requested &&
                   !s_connect_in_progress && !s_sta_connected && !limit_reached && due;
    if (attempt) {
        // Claim the driver before dropping the lock. Python mutation_begin()
        // checks this flag, so neither side can enter the Wi-Fi driver after
        // observing stale permission from an earlier eligibility check.
        s_connect_in_progress = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!attempt) {
        return;
    }
    esp_err_t err = ensure_sta_mode();
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_retry_at_us = 0;
        portEXIT_CRITICAL(&s_lock);
        err = esp_wifi_connect();
    }
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        schedule_retry();
        ESP_LOGW(TAG, "Wi-Fi reconnect request failed (0x%x)", err);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    portENTER_CRITICAL(&s_lock);
    bool connected = s_sta_connected;
    portEXIT_CRITICAL(&s_lock);
    if (!connected) {
        schedule_retry();
    }
}

static esp_err_t disconnect_sta(void)
{
    portENTER_CRITICAL(&s_lock);
    s_sta_connect_requested = false;
    s_connect_in_progress = false;
    s_retry_at_us = 0;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnect request failed (0x%x)", err);
        return err;
    }
    return ESP_OK;
}

static esp_err_t clear_sta_credentials(void)
{
    // WIFI_STORAGE_RAM is shared with Python. Clearing only our NVS record
    // leaves credentials recoverable through WLAN.config()/connect().
    esp_err_t err = ensure_sta_mode();
    if (err == ESP_OK) {
        wifi_config_t empty = {0};
        err = esp_wifi_set_config(WIFI_IF_STA, &empty);
    }
    return err;
}

static esp_err_t stop_sta_interface(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        return ESP_OK;
    }
    if (err != ESP_OK || !mode_has_sta(mode)) {
        return err;
    }

    wifi_mode_t mode_without_sta = (wifi_mode_t)(mode & ~WIFI_MODE_STA);
    if (mode_without_sta != WIFI_MODE_NULL) {
        // In APSTA mode, changing to AP removes STA while leaving Python's AP up.
        return esp_wifi_set_mode(mode_without_sta);
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        return err;
    }
    return esp_wifi_set_mode(WIFI_MODE_NULL);
}

static bool persist_and_publish(const network_record_t *record)
{
    esp_err_t err = persist_record(record);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist network configuration (0x%x)", err);
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    s_persisted = *record;
    s_configuration_error = false;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static void rollback_runtime(const network_record_t *previous)
{
    if (!previous->configured) {
        disconnect_sta();
        if (clear_sta_credentials() != ESP_OK) {
            ESP_LOGW(TAG, "Could not clear uncommitted runtime network configuration");
        }
        portENTER_CRITICAL(&s_lock);
        s_ssid[0] = '\0';
        s_ipv4[0] = '\0';
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (previous->enabled) {
        esp_vision_network_error_t ignored;
        if (connect_record(previous, NETWORK_CONFIG_TIMEOUT_MS, false, &ignored) != ESP_OK) {
            schedule_retry();
        }
        return;
    }
    esp_err_t err = ensure_sta_mode();
    if (err == ESP_OK) {
        wifi_config_t config = config_from_record(previous);
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
        memset(&config, 0, sizeof(config));
    }
    disconnect_sta();
    portENTER_CRITICAL(&s_lock);
    memcpy(s_ssid, previous->ssid, previous->ssid_length);
    s_ssid[previous->ssid_length] = '\0';
    s_ipv4[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not restore the previous runtime configuration (0x%x)", err);
    }
}

static void run_configure(const network_request_t *request)
{
    network_record_t previous;
    network_record_t candidate = {0};
    esp_vision_network_error_t failure = ESP_VISION_NETWORK_ERROR_DRIVER_ERROR;
    portENTER_CRITICAL(&s_lock);
    previous = s_persisted;
    candidate.revision = s_persisted.revision + 1;
    s_runtime_override = false;
    s_sta_runtime_controlled = false;
    s_reconnect_paused = false;
    s_python_reconnect_limit = 0;
    s_python_reconnect_count = 0;
    portEXIT_CRITICAL(&s_lock);
    candidate.configured = 1;
    candidate.enabled = 1;
    candidate.ssid_length = request->ssid_length;
    candidate.password_length = request->password_length;
    memcpy(candidate.ssid, request->ssid, candidate.ssid_length);
    memcpy(candidate.password, request->password, candidate.password_length);

    disconnect_sta();
    esp_err_t err = connect_record(&candidate, NETWORK_CONFIG_TIMEOUT_MS, false, &failure);
    if (err != ESP_OK) {
        rollback_runtime(&previous);
        complete_operation(false, failure == ESP_VISION_NETWORK_ERROR_NONE ?
                           ESP_VISION_NETWORK_ERROR_DRIVER_ERROR : failure);
        memset(&candidate, 0, sizeof(candidate));
        memset(&previous, 0, sizeof(previous));
        return;
    }
    if (!persist_and_publish(&candidate)) {
        rollback_runtime(&previous);
        complete_operation(false, ESP_VISION_NETWORK_ERROR_PERSIST_FAILED);
        memset(&candidate, 0, sizeof(candidate));
        memset(&previous, 0, sizeof(previous));
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_runtime_override = false;
    s_sta_runtime_controlled = false;
    s_reconnect_paused = false;
    s_sta_connect_requested = true;
    s_retry_count = 0;
    s_retry_at_us = 0;
    portEXIT_CRITICAL(&s_lock);
    complete_operation(true, ESP_VISION_NETWORK_ERROR_NONE);
    memset(&candidate, 0, sizeof(candidate));
    memset(&previous, 0, sizeof(previous));
}

static void run_enable(const network_request_t *request)
{
    network_record_t updated;
    portENTER_CRITICAL(&s_lock);
    updated = s_persisted;
    portEXIT_CRITICAL(&s_lock);
    if (!updated.configured && request->enabled) {
        complete_operation(false, ESP_VISION_NETWORK_ERROR_DRIVER_ERROR);
        memset(&updated, 0, sizeof(updated));
        return;
    }
    updated.enabled = request->enabled;
    updated.revision++;
    if (!persist_and_publish(&updated)) {
        complete_operation(false, ESP_VISION_NETWORK_ERROR_PERSIST_FAILED);
        memset(&updated, 0, sizeof(updated));
        return;
    }

    esp_err_t driver_error = ESP_OK;
    if (request->enabled) {
        // Python WLAN.config/connect may have left a transient driver profile.
        // Enabling the background service must restore the committed profile
        // before handing reconnect ownership back to the service.
        driver_error = ensure_sta_mode();
        wifi_config_t current = {0};
        if (driver_error == ESP_OK) {
            driver_error = esp_wifi_get_config(WIFI_IF_STA, &current);
        }
        bool same_profile = false;
        if (driver_error == ESP_OK) {
            size_t current_ssid_length = strnlen((const char *)current.sta.ssid,
                                                 sizeof(current.sta.ssid));
            size_t current_password_length = strnlen((const char *)current.sta.password,
                                                     sizeof(current.sta.password));
            same_profile = current_ssid_length == updated.ssid_length &&
                           current_password_length == updated.password_length &&
                           memcmp(current.sta.ssid, updated.ssid, updated.ssid_length) == 0 &&
                           memcmp(current.sta.password, updated.password,
                                  updated.password_length) == 0;
        }
        memset(&current, 0, sizeof(current));
        if (driver_error == ESP_OK && !same_profile) {
            driver_error = disconnect_sta();
            if (driver_error == ESP_OK) {
                wifi_config_t config = config_from_record(&updated);
                driver_error = esp_wifi_set_config(WIFI_IF_STA, &config);
                memset(&config, 0, sizeof(config));
            }
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_runtime_override = driver_error != ESP_OK;
    s_sta_runtime_controlled = driver_error != ESP_OK;
    s_reconnect_paused = false;
    s_retry_count = 0;
    s_python_reconnect_limit = 0;
    if (!request->enabled || driver_error != ESP_OK) {
        s_sta_connect_requested = false;
    }
    if (request->enabled && driver_error == ESP_OK) {
        // The driver now uses the committed profile. Publish its SSID too,
        // replacing any cached name from a temporary Python configuration.
        memset(s_ssid, 0, sizeof(s_ssid));
        memcpy(s_ssid, updated.ssid, updated.ssid_length);
        s_sta_connect_requested = true;
        s_retry_at_us = 0;
    }
    s_configuration_error = driver_error != ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    if (driver_error == ESP_OK && !request->enabled) {
        driver_error = disconnect_sta();
        if (driver_error != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_runtime_override = true;
            s_sta_runtime_controlled = true;
            s_configuration_error = true;
            portEXIT_CRITICAL(&s_lock);
        }
    }
    if (driver_error != ESP_OK) {
        // The committed enable state is authoritative. Expose any runtime
        // failure without reconnecting with credentials outside that record.
        complete_operation(false, ESP_VISION_NETWORK_ERROR_DRIVER_ERROR);
    } else {
        complete_operation(true, ESP_VISION_NETWORK_ERROR_NONE);
    }
    memset(&updated, 0, sizeof(updated));
}

static void run_forget(void)
{
    network_record_t empty = {0};
    portENTER_CRITICAL(&s_lock);
    empty.revision = s_persisted.revision + 1;
    portEXIT_CRITICAL(&s_lock);
    // Commit the forget intent before changing the live driver. A storage
    // failure therefore leaves the previous persisted and runtime state alone.
    esp_err_t persist_error = persist_record(&empty);
    if (persist_error != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist network configuration (0x%x)", persist_error);
        complete_operation(false, ESP_VISION_NETWORK_ERROR_PERSIST_FAILED);
        memset(&empty, 0, sizeof(empty));
        return;
    }

    // NVS is now authoritative. If a driver operation fails, retain the
    // committed empty record and report the runtime mismatch instead of
    // silently restoring the forgotten credentials.
    (void)disconnect_sta();
    esp_err_t clear_error = clear_sta_credentials();
    esp_err_t stop_error = stop_sta_interface();
    bool runtime_cleared = clear_error == ESP_OK && stop_error == ESP_OK;
    if (clear_error != ESP_OK) {
        ESP_LOGW(TAG, "Could not clear Wi-Fi driver credentials (0x%x)", clear_error);
    }
    if (stop_error != ESP_OK) {
        ESP_LOGW(TAG, "Could not stop the Wi-Fi station interface (0x%x)", stop_error);
    }
    portENTER_CRITICAL(&s_lock);
    s_persisted = empty;
    s_reconnect_paused = false;
    s_sta_connect_requested = false;
    s_configuration_error = !runtime_cleared;
    s_runtime_override = !runtime_cleared;
    s_sta_runtime_controlled = !runtime_cleared;
    if (runtime_cleared) {
        s_sta_active = false;
        s_sta_connected = false;
        s_sta_associated = false;
        s_ssid[0] = '\0';
        s_ipv4[0] = '\0';
    }
    s_retry_count = 0;
    s_retry_at_us = 0;
    s_python_reconnect_limit = 0;
    portEXIT_CRITICAL(&s_lock);
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE | NETWORK_EVENT_DISCONNECTED);
    }
    complete_operation(runtime_cleared,
                       runtime_cleared ? ESP_VISION_NETWORK_ERROR_NONE :
                       ESP_VISION_NETWORK_ERROR_DRIVER_ERROR);
    memset(&empty, 0, sizeof(empty));
}

static void run_operation(void)
{
    network_request_t request;
    int64_t deadline;
    portENTER_CRITICAL(&s_lock);
    if (!s_has_operation || s_operation.state != ESP_VISION_NETWORK_OPERATION_QUEUED) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    request = s_active_request;
    deadline = s_reply_deadline_us;
    portEXIT_CRITICAL(&s_lock);

    while (esp_timer_get_time() < deadline) {
        portENTER_CRITICAL(&s_lock);
        bool released = s_reply_released;
        portEXIT_CRITICAL(&s_lock);
        if (released) {
            break;
        }
        xEventGroupWaitBits(s_events, NETWORK_EVENT_WAKE, pdTRUE, pdFALSE, pdMS_TO_TICKS(25));
    }
    notify_operation_change(ESP_VISION_NETWORK_OPERATION_RUNNING, ESP_VISION_NETWORK_ERROR_NONE);
    switch (request.action) {
    case ESP_VISION_NETWORK_ACTION_CONFIGURE:
        run_configure(&request);
        break;
    case ESP_VISION_NETWORK_ACTION_ENABLE:
        run_enable(&request);
        break;
    case ESP_VISION_NETWORK_ACTION_FORGET:
        run_forget();
        break;
    default:
        complete_operation(false, ESP_VISION_NETWORK_ERROR_DRIVER_ERROR);
        break;
    }
    memset(&request, 0, sizeof(request));
}

static void network_worker(void *arg)
{
    network_record_t initial;
    portENTER_CRITICAL(&s_lock);
    bool auto_connect = s_persisted.configured && s_persisted.enabled &&
                        !s_has_operation && !s_python_mutation_active &&
                        !s_connect_in_progress && !s_sta_connected;
    initial = s_persisted;
    if (auto_connect) {
        s_sta_connect_requested = true;
        s_connect_in_progress = true;
        s_retry_at_us = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    if (auto_connect) {
        esp_vision_network_error_t failure;
        if (connect_record(&initial, NETWORK_CONFIG_TIMEOUT_MS, true, &failure) != ESP_OK) {
            schedule_retry();
        }
    }
    memset(&initial, 0, sizeof(initial));
    notify_changed();

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_events,
                                               NETWORK_EVENT_WAKE | NETWORK_EVENT_DISCONNECTED | NETWORK_EVENT_CONNECTED,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
        if (bits != 0) {
            notify_changed();
            if ((bits & NETWORK_EVENT_CONNECTED) != 0) {
                esp_vision_network_changed_cb_t callback;
                void *context;
                esp_vision_network_snapshot_t snapshot;
                portENTER_CRITICAL(&s_lock);
                callback = s_ip_ready_callback;
                context = s_ip_ready_context;
                snapshot_locked(&snapshot);
                portEXIT_CRITICAL(&s_lock);
                if (callback != NULL) {
                    callback(&snapshot, context);
                }
            }
        }
        portENTER_CRITICAL(&s_lock);
        bool pending = s_has_operation && s_operation.state == ESP_VISION_NETWORK_OPERATION_QUEUED;
        int64_t retry_at = s_retry_at_us;
        portEXIT_CRITICAL(&s_lock);
        if (pending) {
            run_operation();
        } else if (should_reconnect() && (retry_at == 0 || retry_at <= esp_timer_get_time())) {
            try_reconnect();
        }
    }
}

esp_err_t esp_vision_network_start(void)
{
    if (!ESP_VISION_ENABLE_BACKGROUND_NETWORK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = esp_vision_network_init();
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    bool started = s_started;
    portEXIT_CRITICAL(&s_lock);
    if (started) {
        return ESP_OK;
    }
    restore_record_from_nvs();
    TaskHandle_t task = NULL;
    if (xTaskCreate(network_worker, "ev_network", NETWORK_TASK_STACK_SIZE, NULL,
                    NETWORK_TASK_PRIORITY, &task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    s_worker = task;
    s_started = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static bool write_status(char *output, size_t capacity, const esp_vision_network_snapshot_t *snapshot)
{
    if (output == NULL || capacity == 0) {
        return false;
    }
    size_t length = 0;
    output[0] = '\0';
    json_append(output, capacity, &length,
                "{\"ok\":true,\"action\":\"network.status\",\"supported\":");
    json_append(output, capacity, &length, snapshot->supported ? "true" : "false");
    json_append(output, capacity, &length, ",\"enabled\":");
    json_append(output, capacity, &length, snapshot->enabled ? "true" : "false");
    json_append(output, capacity, &length, ",\"configured\":");
    json_append(output, capacity, &length, snapshot->configured ? "true" : "false");
    json_append(output, capacity, &length, ",\"state\":");
    const char *state = state_name(snapshot->state);
    json_append_string(output, capacity, &length, state, strlen(state));
    json_append(output, capacity, &length, ",\"ssid\":");
    if (snapshot->ssid[0] == '\0') {
        json_append(output, capacity, &length, "null");
    } else {
        json_append_string(output, capacity, &length, snapshot->ssid,
                           strnlen(snapshot->ssid, sizeof(snapshot->ssid)));
    }
    json_append(output, capacity, &length, ",\"ipv4\":");
    if (snapshot->ipv4[0] == '\0') {
        json_append(output, capacity, &length, "null");
    } else {
        json_append_string(output, capacity, &length, snapshot->ipv4,
                           strnlen(snapshot->ipv4, sizeof(snapshot->ipv4)));
    }
    json_append_format(output, capacity, &length,
                       ",\"configRevision\":%" PRIu32 ",\"runtimeOverride\":%s,\"configurationError\":%s,\"operation\":",
                       snapshot->config_revision, snapshot->runtime_override ? "true" : "false",
                       snapshot->configuration_error ? "true" : "false");
    if (snapshot->has_operation) {
        write_operation(output, capacity, &length, &snapshot->operation);
    } else {
        json_append(output, capacity, &length, "null");
    }
    json_append(output, capacity, &length, ",\"lastOperation\":");
    if (snapshot->has_last_operation) {
        write_operation(output, capacity, &length, &snapshot->last_operation);
    } else {
        json_append(output, capacity, &length, "null");
    }
    json_append(output, capacity, &length, "}");
    return length < capacity;
}

static bool make_request(const network_json_request_t *json, network_request_t *request,
                         const char **error_code, const char **error_text)
{
    memset(request, 0, sizeof(*request));
    if (!lower_hex_operation_id(json->operation_id)) {
        *error_code = "INVALID_ARGUMENT";
        *error_text = "operationId must be 32 lowercase hexadecimal characters";
        return false;
    }
    memcpy(request->id, json->operation_id, sizeof(request->id));
    if (strcmp(json->action, "network.configure") == 0) {
        request->action = ESP_VISION_NETWORK_ACTION_CONFIGURE;
        if (!json->has_ssid || !json->has_password || json->has_enabled || json->ssid_length == 0 ||
                json->ssid_length > NETWORK_SSID_MAX ||
                !password_valid(json->password, json->password_length)) {
            *error_code = "INVALID_ARGUMENT";
            *error_text = "SSID or password is outside the supported format";
            return false;
        }
        request->ssid_length = json->ssid_length;
        request->password_length = json->password_length;
        memcpy(request->ssid, json->ssid, request->ssid_length);
        memcpy(request->password, json->password, request->password_length);
    } else if (strcmp(json->action, "network.enable") == 0) {
        request->action = ESP_VISION_NETWORK_ACTION_ENABLE;
        if (!json->has_enabled || json->has_ssid || json->has_password) {
            *error_code = "INVALID_ARGUMENT";
            *error_text = "enabled must be a boolean";
            return false;
        }
        request->enabled = json->enabled;
    } else if (strcmp(json->action, "network.forget") == 0) {
        request->action = ESP_VISION_NETWORK_ACTION_FORGET;
        if (json->has_enabled || json->has_ssid || json->has_password) {
            *error_code = "INVALID_ARGUMENT";
            *error_text = "network.forget takes no parameters";
            return false;
        }
    } else {
        *error_code = "UNSUPPORTED_OPERATION";
        *error_text = "Unsupported network action";
        return false;
    }
    if (!request_fingerprint(request)) {
        *error_code = "INTERNAL_ERROR";
        *error_text = "Could not calculate operation identity";
        return false;
    }
    return true;
}

bool esp_vision_network_rpc_handle(const char *request_json, size_t request_len, char *response_json,
                                   size_t response_capacity, bool *response_barrier, char operation_id[33])
{
    if (response_barrier != NULL) {
        *response_barrier = false;
    }
    if (operation_id != NULL) {
        operation_id[0] = '\0';
    }
    if (response_json == NULL || response_capacity == 0) {
        return false;
    }
    network_json_request_t json_request;
    if (!parse_network_json(request_json, request_len, &json_request)) {
        if (contains_network_action(request_json, request_len)) {
            write_rpc_error(response_json, response_capacity, "INVALID_ARGUMENT", "Malformed network action JSON");
            return true;
        }
        return false;
    }
    if (response_capacity < NETWORK_RPC_RESPONSE_MIN_CAPACITY) {
        write_rpc_error(response_json, response_capacity, "INTERNAL_ERROR",
                        "Network response buffer must be at least 1024 bytes");
        memset(&json_request, 0, sizeof(json_request));
        return true;
    }
    if (strcmp(json_request.action, "network.status") == 0) {
        if (json_request.has_operation_id || json_request.has_ssid ||
                json_request.has_password || json_request.has_enabled) {
            write_rpc_error(response_json, response_capacity, "INVALID_ARGUMENT",
                            "network.status takes no parameters");
            return true;
        }
        esp_vision_network_snapshot_t snapshot;
        if (esp_vision_network_snapshot(&snapshot) != ESP_OK) {
            write_rpc_error(response_json, response_capacity, "INTERNAL_ERROR", "Could not read network status");
        } else if (!write_status(response_json, response_capacity, &snapshot)) {
            write_rpc_error(response_json, response_capacity, "INTERNAL_ERROR",
                            "Could not encode network status");
        }
        return true;
    }

    network_request_t request;
    const char *code = "INVALID_ARGUMENT";
    const char *message = "Invalid network action parameters";
    if (!json_request.has_operation_id || !make_request(&json_request, &request, &code, &message)) {
        write_rpc_error(response_json, response_capacity, code, message);
        memset(&request, 0, sizeof(request));
        memset(&json_request, 0, sizeof(json_request));
        return true;
    }
    bool fresh = false;
    bool conflict = false;
    bool busy = false;
    bool not_configured = false;
    bool python_busy = false;
    bool unavailable = false;
    esp_vision_network_operation_t operation = {0};
    portENTER_CRITICAL(&s_lock);
    if (!s_initialized || !s_started) {
        unavailable = true;
    } else if (s_python_mutation_active) {
        python_busy = true;
    } else {
        network_json_operation_match_t match = network_json_operation_match(
                                                   request.id, request.action, request.parameter_hash,
                                                   s_has_operation, s_active_request.id, s_active_request.action, s_active_request.parameter_hash,
                                                   s_has_last_operation, s_last_request.id, s_last_request.action, s_last_request.parameter_hash);
        if (match == NETWORK_JSON_OPERATION_DUPLICATE_ACTIVE) {
            operation = s_operation;
        } else if (match == NETWORK_JSON_OPERATION_DUPLICATE_LAST) {
            operation = s_last_operation;
        } else if (match == NETWORK_JSON_OPERATION_ID_CONFLICT) {
            conflict = true;
        } else if (match == NETWORK_JSON_OPERATION_BUSY) {
            busy = true;
        } else if (request.action == ESP_VISION_NETWORK_ACTION_ENABLE &&
                   request.enabled && !s_persisted.configured) {
            not_configured = true;
        } else {
            s_active_request = request;
            memset(&s_operation, 0, sizeof(s_operation));
            memcpy(s_operation.id, request.id, sizeof(s_operation.id));
            s_operation.action = request.action;
            s_operation.state = ESP_VISION_NETWORK_OPERATION_QUEUED;
            s_has_operation = true;
            s_reply_released = false;
            s_reply_deadline_us = esp_timer_get_time() + (int64_t)NETWORK_ACK_TIMEOUT_MS * 1000;
            operation = s_operation;
            fresh = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (unavailable) {
        write_rpc_error(response_json, response_capacity, "UNSUPPORTED_OPERATION",
                        "Background Wi-Fi service is unavailable");
    } else if (python_busy) {
        write_rpc_error(response_json, response_capacity, "NETWORK_BUSY", "A Python WLAN change is active");
    } else if (conflict) {
        write_rpc_error(response_json, response_capacity, "INVALID_ARGUMENT",
                        "operationId was already used with different parameters");
    } else if (busy) {
        write_rpc_error(response_json, response_capacity, "NETWORK_BUSY", "Another network operation is active");
    } else if (not_configured) {
        write_rpc_error(response_json, response_capacity, "NOT_CONFIGURED",
                        "Configure a network before enabling it");
    } else {
        size_t length = 0;
        response_json[0] = '\0';
        json_append(response_json, response_capacity, &length, "{\"ok\":true,\"action\":");
        const char *action = action_name(operation.action);
        json_append_string(response_json, response_capacity, &length, action, strlen(action));
        json_append(response_json, response_capacity, &length, ",\"accepted\":true,\"operationId\":");
        json_append_string(response_json, response_capacity, &length, operation.id, strlen(operation.id));
        json_append(response_json, response_capacity, &length, ",\"state\":");
        const char *state = operation_state_name(operation.state);
        json_append_string(response_json, response_capacity, &length, state, strlen(state));
        json_append(response_json, response_capacity, &length, "}");
        if (fresh) {
            if (response_barrier != NULL) {
                *response_barrier = true;
            }
            if (operation_id != NULL) {
                memcpy(operation_id, operation.id, sizeof(operation.id));
            }
            if (s_events != NULL) {
                xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE);
            }
        }
    }
    memset(&request, 0, sizeof(request));
    memset(&json_request, 0, sizeof(json_request));
    return true;
}

void esp_vision_network_rpc_response_sent(const char *operation_id)
{
    if (!lower_hex_operation_id(operation_id)) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_has_operation && s_operation.state == ESP_VISION_NETWORK_OPERATION_QUEUED &&
            strcmp(operation_id, s_operation.id) == 0) {
        s_reply_released = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE);
    }
}

esp_err_t esp_vision_network_python_mutation_begin(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_has_operation || s_python_mutation_active || s_connect_in_progress) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_python_mutation_active = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void esp_vision_network_python_mutation_cancel(void)
{
    portENTER_CRITICAL(&s_lock);
    s_python_mutation_active = false;
    portEXIT_CRITICAL(&s_lock);
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE);
    }
}

void esp_vision_network_python_mutation_end(void)
{
    portENTER_CRITICAL(&s_lock);
    s_python_mutation_active = false;
    s_runtime_override = true;
    s_retry_count = 0;
    s_retry_at_us = 0;
    portEXIT_CRITICAL(&s_lock);
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE);
    }
}

void esp_vision_network_python_set_connect_requested(bool requested)
{
    portENTER_CRITICAL(&s_lock);
    s_sta_connect_requested = requested;
    s_runtime_override = true;
    s_sta_runtime_controlled = true;
    s_reconnect_paused = false;
    if (requested) {
        s_python_reconnect_count = 0;
        s_connect_in_progress = true;
        s_retry_at_us = 0;
    } else {
        s_retry_at_us = 0;
        s_connect_in_progress = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE);
    }
}

void esp_vision_network_python_set_sta_active(bool active)
{
    portENTER_CRITICAL(&s_lock);
    s_sta_active = active;
    if (!active) {
        if (s_sta_connect_requested || s_persisted.enabled) {
            s_reconnect_paused = true;
        }
        s_sta_connect_requested = false;
        s_sta_runtime_controlled = true;
        s_runtime_override = true;
        s_retry_at_us = 0;
        s_connect_in_progress = false;
        s_sta_connected = false;
        s_sta_associated = false;
        s_ipv4[0] = '\0';
    } else if (s_reconnect_paused || s_persisted.enabled) {
        s_sta_connect_requested = true;
        s_reconnect_paused = false;
        s_sta_runtime_controlled = true;
        s_runtime_override = true;
        s_retry_at_us = esp_timer_get_time() + 1000000;
    }
    portEXIT_CRITICAL(&s_lock);
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE | (active ? 0 : NETWORK_EVENT_DISCONNECTED));
    }
}

void esp_vision_network_python_set_ap_active(bool active)
{
    portENTER_CRITICAL(&s_lock);
    s_ap_active = active;
    s_runtime_override = true;
    portEXIT_CRITICAL(&s_lock);
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, NETWORK_EVENT_WAKE);
    }
}

void esp_vision_network_python_set_reconnect_limit(uint8_t max_attempts)
{
    portENTER_CRITICAL(&s_lock);
    s_python_reconnect_limit = max_attempts;
    s_python_reconnect_count = 0;
    portEXIT_CRITICAL(&s_lock);
}

void esp_vision_network_python_runtime_changed(void)
{
    wifi_config_t current = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &current) == ESP_OK) {
        size_t length = strnlen((const char *)current.sta.ssid, sizeof(current.sta.ssid));
        portENTER_CRITICAL(&s_lock);
        memset(s_ssid, 0, sizeof(s_ssid));
        size_t copy_length = length < sizeof(s_ssid) - 1 ? length : sizeof(s_ssid) - 1;
        memcpy(s_ssid, current.sta.ssid, copy_length);
        s_runtime_override = true;
        s_sta_runtime_controlled = true;
        portEXIT_CRITICAL(&s_lock);
    }
    memset(&current, 0, sizeof(current));
}

bool esp_vision_network_python_sta_connected(void)
{
    portENTER_CRITICAL(&s_lock);
    bool value = s_sta_connected;
    portEXIT_CRITICAL(&s_lock);
    return value;
}

bool esp_vision_network_python_sta_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool value = s_sta_active;
    portEXIT_CRITICAL(&s_lock);
    return value;
}

bool esp_vision_network_python_ap_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool value = s_ap_active;
    portEXIT_CRITICAL(&s_lock);
    return value;
}

bool esp_vision_network_python_sta_connect_requested(void)
{
    portENTER_CRITICAL(&s_lock);
    bool value = s_sta_connect_requested;
    portEXIT_CRITICAL(&s_lock);
    return value;
}

uint8_t esp_vision_network_python_sta_disconnect_reason(void)
{
    portENTER_CRITICAL(&s_lock);
    uint8_t value = s_disconnect_reason;
    portEXIT_CRITICAL(&s_lock);
    return value;
}

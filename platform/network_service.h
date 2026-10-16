/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Network state is deliberately separate from operation state. */
typedef enum {
    ESP_VISION_NETWORK_UNSUPPORTED = 0,
    ESP_VISION_NETWORK_UNCONFIGURED,
    ESP_VISION_NETWORK_DISABLED,
    ESP_VISION_NETWORK_CONNECTING,
    ESP_VISION_NETWORK_ONLINE,
    ESP_VISION_NETWORK_RETRY_WAIT,
} esp_vision_network_state_t;

typedef enum {
    ESP_VISION_NETWORK_OPERATION_NONE = 0,
    ESP_VISION_NETWORK_OPERATION_QUEUED,
    ESP_VISION_NETWORK_OPERATION_RUNNING,
    ESP_VISION_NETWORK_OPERATION_SUCCEEDED,
    ESP_VISION_NETWORK_OPERATION_FAILED,
} esp_vision_network_operation_state_t;

typedef enum {
    ESP_VISION_NETWORK_ACTION_NONE = 0,
    ESP_VISION_NETWORK_ACTION_CONFIGURE,
    ESP_VISION_NETWORK_ACTION_ENABLE,
    ESP_VISION_NETWORK_ACTION_FORGET,
} esp_vision_network_action_t;

typedef enum {
    ESP_VISION_NETWORK_ERROR_NONE = 0,
    ESP_VISION_NETWORK_ERROR_AUTH_FAILED,
    ESP_VISION_NETWORK_ERROR_AP_NOT_FOUND,
    ESP_VISION_NETWORK_ERROR_CONNECT_TIMEOUT,
    ESP_VISION_NETWORK_ERROR_IP_TIMEOUT,
    ESP_VISION_NETWORK_ERROR_PERSIST_FAILED,
    ESP_VISION_NETWORK_ERROR_DRIVER_ERROR,
} esp_vision_network_error_t;

typedef struct {
    char id[33];
    esp_vision_network_action_t action;
    esp_vision_network_operation_state_t state;
    esp_vision_network_error_t error;
} esp_vision_network_operation_t;

/* A status snapshot never contains a password or credential fingerprint. */
typedef struct {
    bool supported;
    bool enabled;          /* Persisted background-service setting. */
    bool configured;
    bool configuration_error;
    bool runtime_override; /* Python changed the active, RAM-only runtime. */
    esp_vision_network_state_t state;
    uint32_t config_revision;
    char ssid[33];
    char ipv4[16];
    bool has_operation;
    bool has_last_operation;
    esp_vision_network_operation_t operation;
    esp_vision_network_operation_t last_operation;

    /* MicroPython compatibility state; these fields are not RPC output. */
    bool sta_active;
    bool ap_active;
    bool sta_connected;
    bool sta_connect_requested;
    uint8_t sta_disconnect_reason;
    uint8_t sta_reconnect_count;
} esp_vision_network_snapshot_t;

typedef void (*esp_vision_network_changed_cb_t)(const esp_vision_network_snapshot_t *snapshot,
                                                void *context);

/*
 * Idempotently initializes esp-netif, the default WiFi event loop, the STA/AP
 * netifs, the WiFi driver and this module's sole WiFi/IP event handlers. It is
 * safe for both platform startup and MicroPython's network module to call it.
 * NVS must already be initialized. The default event loop may already exist.
 */
esp_err_t esp_vision_network_init(void);

/* Initializes only shared esp-netif state for non-Wi-Fi network interfaces. */
esp_err_t esp_vision_network_netif_init(void);

/* Starts the persistent-config worker once, restoring the last complete NVS record. */
esp_err_t esp_vision_network_start(void);

/* Copies a lock-consistent snapshot. Callers own the destination storage. */
esp_err_t esp_vision_network_snapshot(esp_vision_network_snapshot_t *snapshot);
esp_netif_t *esp_vision_network_sta_netif(void);
esp_netif_t *esp_vision_network_ap_netif(void);

/*
 * Handle network.* device.control actions from an already-authorized owner.
 * The function parses the complete JSON payload, validates string escapes,
 * UTF-8, duplicate known fields, and value constraints, and writes a complete
 * JSON result. response_capacity must be at least 1024 bytes. It returns true for any network.* action (including malformed
 * actions, which receive INVALID_ARGUMENT) and false for other actions.
 *
 * Fresh modifying requests are held behind a one-second reply barrier. When
 * response_barrier is true, notify the service with the copied operation ID
 * after the accepted reply's TX completion; the service still proceeds after
 * one second if no notification arrives. A duplicate operation does not need
 * a new barrier.
 */
bool esp_vision_network_rpc_handle(const char *request_json,
                                   size_t request_len,
                                   char *response_json,
                                   size_t response_capacity,
                                   bool *response_barrier,
                                   char operation_id[33]);
void esp_vision_network_rpc_response_sent(const char *operation_id);

/* Change notifications run from the network worker and must not call Python. */
void esp_vision_network_set_changed_callback(esp_vision_network_changed_cb_t callback,
                                             void *context);
void esp_vision_network_set_ip_ready_callback(esp_vision_network_changed_cb_t callback,
                                              void *context);

/*
 * Python WLAN mutations share the background operation gate. Begin returns
 * ESP_ERR_INVALID_STATE while an asynchronous configure/enable/forget is in
 * progress, which wrappers translate to OSError(EBUSY). Every successful
 * begin must have a matching end on all paths. End marks runtimeOverride and
 * keeps Python's runtime changes out of the persistent record.
 */
esp_err_t esp_vision_network_python_mutation_begin(void);
void esp_vision_network_python_mutation_cancel(void);
void esp_vision_network_python_mutation_end(void);

/* Python is the explicit source of transient reconnect intent. */
void esp_vision_network_python_set_connect_requested(bool requested);
void esp_vision_network_python_set_sta_active(bool active);
void esp_vision_network_python_set_ap_active(bool active);
void esp_vision_network_python_set_reconnect_limit(uint8_t max_attempts);
void esp_vision_network_python_runtime_changed(void);

/* Compatibility getters backed by the shared event state. */
bool esp_vision_network_python_sta_connected(void);
bool esp_vision_network_python_sta_active(void);
bool esp_vision_network_python_ap_active(void);
bool esp_vision_network_python_sta_connect_requested(void);
uint8_t esp_vision_network_python_sta_disconnect_reason(void);

#ifdef __cplusplus
}
#endif

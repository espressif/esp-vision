/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ev_mux.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define EV_MUX_SINK_LOCK_COUNT (5)
// Total budget for one frame segment; writes only retry while progressing.
#define EV_MUX_WRITE_ALL_TIMEOUT_MS (1500)
// A stalled sink stays "congested" for this long. While congested, lossy
// streams (preview) drop whole frames before writing a single byte instead
// of tearing a partial frame onto the wire. After the cooldown the next
// write acts as a probe: it either completes (congestion clears) or stalls
// again (congestion re-arms).
#define EV_MUX_CONGESTION_COOLDOWN_MS (500)

// Incremented on every frame-segment write failure (see
// ev_control_transport.c "transport.stats").
static uint32_t s_tx_timeout_count;
// Whole frames dropped at entry because the sink was congested.
static uint32_t s_tx_drop_count;

typedef struct {
    const char *sink_id;
    SemaphoreHandle_t mutex;
    volatile bool congested;
    uint32_t congested_at_ms;
} ev_mux_sink_lock_t;

static ev_mux_sink_lock_t s_sink_locks[EV_MUX_SINK_LOCK_COUNT] = {
    { "usj", NULL, false, 0 },
    { "cdc", NULL, false, 0 },
    { "uart", NULL, false, 0 },
    { "console", NULL, false, 0 },
    { "null", NULL, false, 0 },
};

esp_err_t ev_mux_init0(void)
{
    for (size_t i = 0; i < EV_MUX_SINK_LOCK_COUNT; ++i) {
        if (s_sink_locks[i].mutex == NULL) {
            s_sink_locks[i].mutex = xSemaphoreCreateRecursiveMutex();
            if (s_sink_locks[i].mutex == NULL) {
                return ESP_ERR_NO_MEM;
            }
        }
    }
    return ESP_OK;
}

static ev_mux_sink_lock_t *ev_mux_sink_entry(const char *sink_id)
{
    const char *normalized = (strcmp(sink_id, "none") == 0) ? "null" : sink_id;
    for (size_t i = 0; i < EV_MUX_SINK_LOCK_COUNT; ++i) {
        if (strcmp(normalized, s_sink_locks[i].sink_id) == 0) {
            return &s_sink_locks[i];
        }
    }
    return NULL;
}

static SemaphoreHandle_t ev_mux_sink_mutex(const char *sink_id)
{
    ev_mux_sink_lock_t *entry = ev_mux_sink_entry(sink_id);
    return (entry != NULL) ? entry->mutex : NULL;
}

static uint32_t ev_mux_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void ev_mux_sink_mark_congested(const char *sink_id)
{
    ev_mux_sink_lock_t *entry = ev_mux_sink_entry(sink_id);
    if (entry != NULL) {
        entry->congested = true;
        entry->congested_at_ms = ev_mux_now_ms();
    }
}

static bool ev_mux_sink_is_congested(const char *sink_id)
{
    ev_mux_sink_lock_t *entry = ev_mux_sink_entry(sink_id);
    if ((entry == NULL) || !entry->congested) {
        return false;
    }
    // Cooldown elapsed: allow one write through as the recovery probe.
    return (ev_mux_now_ms() - entry->congested_at_ms) <= EV_MUX_CONGESTION_COOLDOWN_MS;
}

uint32_t ev_mux_crc32(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

static esp_err_t ev_mux_write_all_sink_if_present(const char *sink_id, const void *buf, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }

    const uint8_t *bytes = (const uint8_t *)buf;
    size_t done = 0;
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);
    while (done < len) {
        ssize_t written = ev_channel_write_sink_if_present(sink_id, bytes + done, len - done);
        if (written <= 0) {
            // No progress: sink offline or wedged. Abort this segment (the
            // frame is dropped, but the failure is explicit, not silent).
            __atomic_fetch_add(&s_tx_timeout_count, 1, __ATOMIC_RELAXED);
            ev_mux_sink_mark_congested(sink_id);
            return ESP_ERR_TIMEOUT;
        }
        done += (size_t)written;
        if ((done < len) &&
                ((uint32_t)(esp_timer_get_time() / 1000) - start > EV_MUX_WRITE_ALL_TIMEOUT_MS)) {
            __atomic_fetch_add(&s_tx_timeout_count, 1, __ATOMIC_RELAXED);
            ev_mux_sink_mark_congested(sink_id);
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

typedef esp_err_t (*ev_mux_write_all_fn_t)(void *ctx, const void *buf, size_t len);

static esp_err_t ev_mux_write_sink_cb(void *ctx, const void *buf, size_t len)
{
    return ev_mux_write_all_sink_if_present((const char *)ctx, buf, len);
}

static esp_err_t ev_mux_write_with(ev_mux_write_all_fn_t write_all,
                                   void *ctx,
                                   const char *metadata,
                                   const void *payload,
                                   size_t payload_len)
{
    if (metadata == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((payload == NULL) && (payload_len != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t metadata_len = strlen(metadata);
    uint32_t payload_crc = ev_mux_crc32(payload, payload_len);
    char header[56];
    int header_len = snprintf(header,
                              sizeof(header),
                              "\x1e"
                              "EVMUX/1 h=%u p=%u c=%08" PRIX32 "\r\n",
                              (unsigned int)metadata_len,
                              (unsigned int)payload_len,
                              payload_crc);
    if ((header_len <= 0) || ((size_t)header_len >= sizeof(header))) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = write_all(ctx, header, (size_t)header_len);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = write_all(ctx, metadata, metadata_len);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = write_all(ctx, payload, payload_len);
    if (ret != ESP_OK) {
        return ret;
    }
    return write_all(ctx, "\x1f", 1);
}

static esp_err_t ev_mux_write_locked_sink(const char *sink_id,
                                          const char *metadata,
                                          const void *payload,
                                          size_t payload_len)
{
    SemaphoreHandle_t mutex = ev_mux_sink_mutex(sink_id);
    if (mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTakeRecursive(mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ev_mux_write_with(ev_mux_write_sink_cb, (void *)sink_id, metadata, payload, payload_len);
    if (ret == ESP_OK) {
        ev_mux_sink_lock_t *entry = ev_mux_sink_entry(sink_id);
        if (entry != NULL) {
            entry->congested = false;
        }
    }
    xSemaphoreGiveRecursive(mutex);
    return ret;
}

esp_err_t ev_mux_write(ev_stream_t stream,
                       const char *metadata,
                       const void *payload,
                       size_t payload_len)
{
    if (stream >= EV_STREAM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ev_mux_init0() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (!ev_channel_ready(stream)) {
        return ESP_ERR_INVALID_STATE;
    }

    // Pin the physical sink for the complete frame. Route changes apply to the next frame.
    const char *sink_id = ev_channel_get(stream);
    return ev_mux_write_locked_sink(sink_id, metadata, payload, payload_len);
}

esp_err_t ev_mux_write_lossy(ev_stream_t stream,
                             const char *metadata,
                             const void *payload,
                             size_t payload_len)
{
    if (stream >= EV_STREAM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ev_mux_init0() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (!ev_channel_ready(stream)) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *sink_id = ev_channel_get(stream);
    if (ev_mux_sink_is_congested(sink_id)) {
        ev_mux_record_tx_drop();
        return ESP_ERR_TIMEOUT;
    }
    return ev_mux_write_locked_sink(sink_id, metadata, payload, payload_len);
}

bool ev_mux_stream_congested(ev_stream_t stream)
{
    if (stream >= EV_STREAM_MAX) {
        return false;
    }
    if (!ev_channel_ready(stream)) {
        return false;
    }
    return ev_mux_sink_is_congested(ev_channel_get(stream));
}

void ev_mux_record_tx_drop(void)
{
    __atomic_fetch_add(&s_tx_drop_count, 1, __ATOMIC_RELAXED);
}

uint32_t ev_mux_tx_timeout_count(void)
{
    return __atomic_load_n(&s_tx_timeout_count, __ATOMIC_RELAXED);
}

uint32_t ev_mux_tx_drop_count(void)
{
    return __atomic_load_n(&s_tx_drop_count, __ATOMIC_RELAXED);
}

esp_err_t ev_mux_write_sink_if_present(const char *sink_id,
                                       const char *metadata,
                                       const void *payload,
                                       size_t payload_len)
{
    if (sink_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ev_mux_init0() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (!ev_channel_sink_present(sink_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ev_mux_write_locked_sink(sink_id, metadata, payload, payload_len);
}

int ev_mux_snprintf_u32(char *buf, size_t len, uint32_t value)
{
    return snprintf(buf, len, "%" PRIu32, value);
}

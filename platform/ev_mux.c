/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ev_mux.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "boardconfig.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#ifndef CONFIG_SPIRAM
#define CONFIG_SPIRAM (0)
#endif
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "esp_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "py/mpconfig.h"

#include "ev_control_json.h"
#include "ev_memory.h"
#include "ev_session.h"

#define EV_MUX_SERIAL_TARGETS (3)
#define EV_MUX_TARGET_COUNT (EV_MUX_SERIAL_TARGETS + EV_TRANSPORT_TCP_LINKS)
#define EV_MUX_QUEUE_BYTES (16 * 1024)
#define EV_MUX_QUEUE_RESERVED_BYTES (4 * 1024)
#define EV_MUX_QUEUE_FRAMES (8)
#define EV_MUX_QUEUE_RESERVED_FRAMES (2)
#define EV_MUX_BLOCK_SIZE (256)
#define EV_MUX_BLOCK_COUNT (EV_MUX_QUEUE_BYTES / EV_MUX_BLOCK_SIZE)
#define EV_MUX_METADATA_MAX (384)
#define EV_MUX_HEADER_MAX (56)
#define EV_MUX_QUEUE_TIMEOUT_MS (2000)
#define EV_MUX_FRAME_TIMEOUT_MS (1500)
#define EV_MUX_REPL_STAGE_BYTES (512)
#define EV_MUX_REPL_COALESCE_MS (5)
#define EV_MUX_UART_FRAME_TIMEOUT_MAX_MS (10000)
#define EV_MUX_CONGESTION_COOLDOWN_MS (500)
#define EV_MUX_CRC_CHUNK_BYTES (512)
#define EV_MUX_WRITE_CHUNK_BYTES (4096)
#define EV_MUX_TICKET_COUNT (8)
#define EV_MUX_TX_TASK_STACK_SIZE (4096)
#define EV_MUX_TX_TASK_PRIORITY (ESP_TASK_PRIO_MIN + 3)
#define EV_MUX_ARENA_BYTES ((EV_MUX_SERIAL_TARGETS + 1) * EV_MUX_QUEUE_BYTES)

#ifndef ESP_VISION_EV_MUX_UART_BAUD
#if defined(MICROPY_HW_UART_REPL_BAUD)
#define ESP_VISION_EV_MUX_UART_BAUD (MICROPY_HW_UART_REPL_BAUD)
#else
#define ESP_VISION_EV_MUX_UART_BAUD (115200)
#endif
#endif

_Static_assert(EV_MUX_BLOCK_COUNT == 64, "EV-MUX queue bitmap expects 64 blocks");

typedef struct {
    bool used;
    bool tcp_quota_reserved;
    bool tcp_discovery_reservation;
    bool tcp_low_reservation;
    uint8_t priority;
    uint8_t block_count;
    uint8_t blocks[EV_MUX_BLOCK_COUNT];
    ev_link_ref_t link;
    char session_id[EV_SESSION_ID_HEX_LEN + 1];
    uint32_t vm_epoch;
    uint32_t output_input_generation;
    uint32_t output_transition_id;
    uint32_t enqueue_ms;
    uint32_t start_ms;
    bool attempted;
    uint32_t enqueue_order;
    uint32_t frame_timeout_ms;
    uint16_t metadata_len;
    char metadata[EV_MUX_METADATA_MAX + 1];
    char header[EV_MUX_HEADER_MAX];
    uint16_t header_len;
    uint32_t payload_len;
    uint32_t payload_crc_state;
    uint32_t payload_crc_pos;
    size_t total_len;
    size_t tx_offset;
    bool header_ready;
    ev_mux_ticket_t ticket;
    ev_mux_completion_cb_t completion;
    uint8_t completion_context_len;
    uint8_t completion_context[EV_MUX_TX_CONTEXT_MAX];
} ev_mux_frame_t;

typedef struct {
    ev_mux_submit_options_t options;
    ev_link_ref_t link;
    char session_id[EV_SESSION_ID_HEX_LEN + 1];
    ev_vm_output_origin_t origin;
} ev_mux_stream_target_t;

typedef struct {
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    uint8_t *arena;
    uint64_t free_blocks;
    size_t bytes_used;
    uint8_t frames_used;
    uint32_t congested_at_ms;
    bool congested;
    ev_mux_frame_t frames[EV_MUX_QUEUE_FRAMES];
} ev_mux_target_queue_t;

typedef struct {
    bool allocated;
    bool done;
    bool forgotten;
    uint32_t generation;
    esp_err_t result;
    SemaphoreHandle_t semaphore;
    StaticSemaphore_t semaphore_storage;
} ev_mux_ticket_state_t;

typedef enum {
    EV_MUX_SPECIAL_NONE = 0,
    EV_MUX_SPECIAL_PREVIEW,
    EV_MUX_SPECIAL_LARGE,
} ev_mux_special_kind_t;

typedef struct {
    bool used;
    ev_mux_special_kind_t kind;
    uint8_t pool_index;
    ev_mux_frame_t frame;
    uint8_t *payload;
} ev_mux_special_frame_t;

typedef struct {
    bool pending;
    ev_mux_completion_cb_t callback;
    ev_link_ref_t link;
    esp_err_t result;
    uint32_t ticket;
    uint8_t context_len;
    uint8_t context[EV_MUX_TX_CONTEXT_MAX];
} ev_mux_completion_t;

typedef struct {
    bool used;
    bool retrying;
    ev_link_ref_t link;
    char session_id[EV_SESSION_ID_HEX_LEN + 1];
    uint32_t vm_epoch;
    uint32_t output_input_generation;
    uint32_t output_transition_id;
    uint32_t last_append_ms;
    uint32_t retry_started_ms;
    uint16_t metadata_len;
    size_t payload_len;
    char metadata[EV_MUX_METADATA_MAX + 1];
    uint8_t payload[EV_MUX_REPL_STAGE_BYTES];
} ev_mux_repl_stage_t;

typedef enum {
    EV_MUX_FRAME_ALLOWED = 0,
    EV_MUX_FRAME_WAIT,
    EV_MUX_FRAME_STALE,
} ev_mux_frame_auth_t;

static uint8_t *s_arena_slab;
static uint8_t *s_tcp_arena;
static ev_mux_target_queue_t s_targets[EV_MUX_TARGET_COUNT];
static ev_mux_ticket_state_t s_tickets[EV_MUX_TICKET_COUNT];
static ev_mux_special_frame_t s_preview_frames[2];
static ev_mux_special_frame_t s_large_frame;
static uintptr_t s_special_partial_frame[EV_MUX_TARGET_COUNT];
static uint8_t *s_preview_buffers[2];
static uint8_t *s_large_buffer;
static int8_t s_preview_pending = -1;
static int8_t s_preview_active = -1;
static SemaphoreHandle_t s_special_mutex;
static StaticSemaphore_t s_special_mutex_storage;
static portMUX_TYPE s_ticket_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_tcp_quota_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_tcp_free_blocks = UINT64_MAX;
static size_t s_tcp_bytes_used;
static uint8_t s_tcp_frames_used;
static size_t s_tcp_discovery_bytes_used;
static uint8_t s_tcp_discovery_frames_used;
static size_t s_tcp_low_bytes_used;
static uint8_t s_tcp_low_frames_used;
static TaskHandle_t s_tx_task;
static StaticTask_t s_tx_task_tcb;
static StackType_t s_tx_task_stack[EV_MUX_TX_TASK_STACK_SIZE / sizeof(StackType_t)];
static bool s_initialized;
static SemaphoreHandle_t s_init_mutex;
static StaticSemaphore_t s_init_mutex_storage;
static portMUX_TYPE s_init_mutex_create_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_repl_producer_mutex;
static StaticSemaphore_t s_repl_producer_mutex_storage;
static SemaphoreHandle_t s_repl_stage_mutex;
static StaticSemaphore_t s_repl_stage_mutex_storage;
static ev_mux_repl_stage_t s_repl_stage;
static uint32_t s_repl_cancelled_epoch;
static uint32_t s_tx_timeout_count;
static uint32_t s_tx_drop_count;
static uint32_t s_tx_queue_full_count;
static uint32_t s_tx_stale_count;
static uint32_t s_tx_enqueue_order;

static void ev_mux_tx_task(void *argument);
static void ev_mux_fail_session(const ev_link_ref_t *link, const char *session_id, bool close_tcp);
static esp_err_t ev_mux_write_repl_target(const ev_mux_stream_target_t *target,
                                          const char *metadata,
                                          const void *payload,
                                          size_t payload_len);
static bool ev_mux_preview_target_available(const ev_mux_stream_target_t *target);
static esp_err_t ev_mux_stream_metadata_bind(const char *metadata,
                                             const ev_mux_submit_options_t *options,
                                             char *output,
                                             size_t *output_len);
static bool ev_mux_link_equal(const ev_link_ref_t *left, const ev_link_ref_t *right);

static bool ev_mux_queue_is_tcp(const ev_mux_target_queue_t *queue)
{
    return (queue != NULL) && (queue->arena == s_tcp_arena);
}

static uint8_t *ev_mux_arena_alloc(void)
{
#if CONFIG_SPIRAM
    // Boards configured for PSRAM must not consume internal memory reserved
    // for DMA when PSRAM is absent or fragmented.
    if (!esp_psram_is_initialized()) {
        return NULL;
    }
    return esp_vision_memory_malloc_caps(EV_MUX_ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    // Preserve the existing internal-memory behavior on boards without PSRAM.
    return esp_vision_memory_malloc_caps(EV_MUX_ARENA_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

static void ev_mux_target_queues_reset(void)
{
    s_tcp_arena = NULL;
    for (size_t i = 0; i < EV_MUX_TARGET_COUNT; ++i) {
        __atomic_store_n(&s_special_partial_frame[i], (uintptr_t)0, __ATOMIC_RELEASE);
        ev_mux_target_queue_t *queue = &s_targets[i];
        queue->arena = NULL;
        queue->free_blocks = 0;
        queue->bytes_used = 0;
        queue->frames_used = 0;
        queue->congested_at_ms = 0;
        queue->congested = false;
        memset(queue->frames, 0, sizeof(queue->frames));
    }
}

static void ev_mux_target_queues_bind(void)
{
    s_tcp_arena = s_arena_slab + (EV_MUX_SERIAL_TARGETS * EV_MUX_QUEUE_BYTES);
    for (size_t i = 0; i < EV_MUX_TARGET_COUNT; ++i) {
        __atomic_store_n(&s_special_partial_frame[i], (uintptr_t)0, __ATOMIC_RELEASE);
        ev_mux_target_queue_t *queue = &s_targets[i];
        queue->arena = (i < EV_MUX_SERIAL_TARGETS) ?
                       (s_arena_slab + (i * EV_MUX_QUEUE_BYTES)) : s_tcp_arena;
        queue->free_blocks = (i < EV_MUX_SERIAL_TARGETS) ? UINT64_MAX : 0;
        queue->bytes_used = 0;
        queue->frames_used = 0;
        queue->congested_at_ms = 0;
        queue->congested = false;
        memset(queue->frames, 0, sizeof(queue->frames));
    }
    portENTER_CRITICAL(&s_tcp_quota_mux);
    s_tcp_free_blocks = UINT64_MAX;
    s_tcp_bytes_used = 0;
    s_tcp_frames_used = 0;
    s_tcp_discovery_bytes_used = 0;
    s_tcp_discovery_frames_used = 0;
    s_tcp_low_bytes_used = 0;
    s_tcp_low_frames_used = 0;
    portEXIT_CRITICAL(&s_tcp_quota_mux);
}

static bool ev_mux_tcp_reserve(ev_mux_frame_t *frame, size_t encoded_len)
{
    bool discovery = frame->session_id[0] == '\0';
    bool low_priority = frame->priority >= EV_MUX_TX_REPL;
    bool reserved = false;

    portENTER_CRITICAL(&s_tcp_quota_mux);
    if ((s_tcp_frames_used < EV_MUX_QUEUE_FRAMES) &&
            ((s_tcp_bytes_used + encoded_len) <= EV_MUX_QUEUE_BYTES) &&
            (!discovery || ((s_tcp_discovery_frames_used <
                             (EV_MUX_QUEUE_FRAMES - EV_MUX_QUEUE_RESERVED_FRAMES)) &&
                            ((s_tcp_discovery_bytes_used + encoded_len) <=
                             (EV_MUX_QUEUE_BYTES - EV_MUX_QUEUE_RESERVED_BYTES)))) &&
            (!low_priority || ((s_tcp_low_frames_used <
                                (EV_MUX_QUEUE_FRAMES - EV_MUX_QUEUE_RESERVED_FRAMES)) &&
                               ((s_tcp_low_bytes_used + encoded_len) <=
                                (EV_MUX_QUEUE_BYTES - EV_MUX_QUEUE_RESERVED_BYTES))))) {
        ++s_tcp_frames_used;
        s_tcp_bytes_used += encoded_len;
        frame->tcp_quota_reserved = true;
        if (discovery) {
            ++s_tcp_discovery_frames_used;
            s_tcp_discovery_bytes_used += encoded_len;
            frame->tcp_discovery_reservation = true;
        }
        if (low_priority) {
            ++s_tcp_low_frames_used;
            s_tcp_low_bytes_used += encoded_len;
            frame->tcp_low_reservation = true;
        }
        reserved = true;
    }
    portEXIT_CRITICAL(&s_tcp_quota_mux);
    return reserved;
}

static void ev_mux_tcp_release_reservation(ev_mux_frame_t *frame)
{
    if ((frame == NULL) || !frame->tcp_quota_reserved) {
        return;
    }
    portENTER_CRITICAL(&s_tcp_quota_mux);
    if (s_tcp_frames_used > 0) {
        --s_tcp_frames_used;
    }
    if (s_tcp_bytes_used >= frame->total_len) {
        s_tcp_bytes_used -= frame->total_len;
    } else {
        s_tcp_bytes_used = 0;
    }
    if (frame->tcp_discovery_reservation) {
        if (s_tcp_discovery_frames_used > 0) {
            --s_tcp_discovery_frames_used;
        }
        if (s_tcp_discovery_bytes_used >= frame->total_len) {
            s_tcp_discovery_bytes_used -= frame->total_len;
        } else {
            s_tcp_discovery_bytes_used = 0;
        }
    }
    if (frame->tcp_low_reservation) {
        if (s_tcp_low_frames_used > 0) {
            --s_tcp_low_frames_used;
        }
        if (s_tcp_low_bytes_used >= frame->total_len) {
            s_tcp_low_bytes_used -= frame->total_len;
        } else {
            s_tcp_low_bytes_used = 0;
        }
    }
    frame->tcp_quota_reserved = false;
    frame->tcp_discovery_reservation = false;
    frame->tcp_low_reservation = false;
    portEXIT_CRITICAL(&s_tcp_quota_mux);
}

static uint32_t ev_mux_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t ev_mux_crc32_update(uint32_t crc, uint8_t value)
{
    crc ^= value;
    for (int bit = 0; bit < 8; ++bit) {
        uint32_t mask = 0u - (crc & 1u);
        crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
    return crc;
}

uint32_t ev_mux_crc32(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; ++i) {
        crc = ev_mux_crc32_update(crc, bytes[i]);
    }
    return ~crc;
}

static int ev_mux_target_index(const ev_link_ref_t *link)
{
    if ((link == NULL) || (link->transport_id >= EV_TRANSPORT_MAX)) {
        return -1;
    }
    if (link->transport_id < EV_TRANSPORT_TCP) {
        return (link->slot == EV_TRANSPORT_PHYSICAL_SLOT) ? (int)link->transport_id : -1;
    }
    if (link->slot >= EV_TRANSPORT_TCP_LINKS) {
        return -1;
    }
    return EV_MUX_SERIAL_TARGETS + link->slot;
}

static uint32_t ev_mux_frame_timeout(const ev_link_ref_t *link, size_t encoded_len)
{
    if (link->transport_id != EV_TRANSPORT_UART) {
        return EV_MUX_FRAME_TIMEOUT_MS;
    }
    uint32_t baud = ESP_VISION_EV_MUX_UART_BAUD;
    if (baud == 0) {
        return 0;
    }
    uint64_t wire_ms = (((uint64_t)encoded_len * 10000u) + baud - 1u) / baud;
    uint64_t timeout_ms = (wire_ms * 2u) + 250u;
    if (timeout_ms < EV_MUX_FRAME_TIMEOUT_MS) {
        timeout_ms = EV_MUX_FRAME_TIMEOUT_MS;
    }
    if (timeout_ms > EV_MUX_UART_FRAME_TIMEOUT_MAX_MS) {
        return 0;
    }
    return (uint32_t)timeout_ms;
}

static uint32_t ev_mux_ticket_encode(uint8_t slot, uint32_t generation)
{
    return ((generation & 0x00ffffffu) << 8) | (uint32_t)(slot + 1u);
}

static bool ev_mux_ticket_decode(ev_mux_ticket_t ticket, uint8_t *slot, uint32_t *generation)
{
    uint8_t encoded_slot = (uint8_t)(ticket & 0xffu);
    if ((ticket == 0) || (encoded_slot == 0) || (encoded_slot > EV_MUX_TICKET_COUNT)) {
        return false;
    }
    *slot = encoded_slot - 1u;
    *generation = ticket >> 8;
    return *generation != 0;
}

static ev_mux_ticket_t ev_mux_ticket_allocate(void)
{
    ev_mux_ticket_t ticket = 0;
    portENTER_CRITICAL(&s_ticket_mux);
    for (uint8_t i = 0; i < EV_MUX_TICKET_COUNT; ++i) {
        ev_mux_ticket_state_t *state = &s_tickets[i];
        if (!state->allocated) {
            state->generation = (state->generation + 1u) & 0x00ffffffu;
            if (state->generation == 0) {
                state->generation = 1;
            }
            state->allocated = true;
            state->done = false;
            state->forgotten = false;
            state->result = ESP_ERR_INVALID_STATE;
            xSemaphoreTake(state->semaphore, 0);
            ticket = ev_mux_ticket_encode(i, state->generation);
            break;
        }
    }
    portEXIT_CRITICAL(&s_ticket_mux);
    return ticket;
}

static ev_mux_ticket_state_t *ev_mux_ticket_lookup_locked(ev_mux_ticket_t ticket)
{
    uint8_t slot;
    uint32_t generation;
    if (!ev_mux_ticket_decode(ticket, &slot, &generation)) {
        return NULL;
    }
    ev_mux_ticket_state_t *state = &s_tickets[slot];
    if (!state->allocated || (state->generation != generation)) {
        return NULL;
    }
    return state;
}

static void ev_mux_ticket_complete(ev_mux_ticket_t ticket, esp_err_t result)
{
    if (ticket == 0) {
        return;
    }
    SemaphoreHandle_t semaphore = NULL;
    portENTER_CRITICAL(&s_ticket_mux);
    ev_mux_ticket_state_t *state = ev_mux_ticket_lookup_locked(ticket);
    if (state != NULL) {
        state->done = true;
        state->result = result;
        if (state->forgotten) {
            state->allocated = false;
        } else {
            semaphore = state->semaphore;
        }
    }
    portEXIT_CRITICAL(&s_ticket_mux);
    if (semaphore != NULL) {
        xSemaphoreGive(semaphore);
    }
}

esp_err_t ev_mux_wait(ev_mux_ticket_t ticket, uint32_t timeout_ms, esp_err_t *tx_result_out)
{
    if (tx_result_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t slot;
    uint32_t generation;
    if (!ev_mux_ticket_decode(ticket, &slot, &generation)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_ticket_mux);
    ev_mux_ticket_state_t *state = ev_mux_ticket_lookup_locked(ticket);
    bool done = (state != NULL) && state->done;
    SemaphoreHandle_t semaphore = (state != NULL) ? state->semaphore : NULL;
    portEXIT_CRITICAL(&s_ticket_mux);
    if (state == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!done) {
        TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
        if ((timeout_ms != 0) && (ticks == 0)) {
            ticks = 1;
        }
        if ((semaphore == NULL) || (xSemaphoreTake(semaphore, ticks) != pdTRUE)) {
            return ESP_ERR_TIMEOUT;
        }
    }

    portENTER_CRITICAL(&s_ticket_mux);
    state = ev_mux_ticket_lookup_locked(ticket);
    done = (state != NULL) && state->done;
    if (done) {
        *tx_result_out = state->result;
    }
    portEXIT_CRITICAL(&s_ticket_mux);
    return done ? ESP_OK : ESP_ERR_TIMEOUT;
}

void ev_mux_ticket_forget(ev_mux_ticket_t ticket)
{
    portENTER_CRITICAL(&s_ticket_mux);
    ev_mux_ticket_state_t *state = ev_mux_ticket_lookup_locked(ticket);
    if (state != NULL) {
        if (state->done) {
            state->allocated = false;
        } else {
            state->forgotten = true;
        }
    }
    portEXIT_CRITICAL(&s_ticket_mux);
}

static void ev_mux_completion_prepare(const ev_mux_frame_t *frame,
                                      esp_err_t result,
                                      ev_mux_completion_t *completion)
{
    ev_mux_ticket_complete(frame->ticket, result);
    if (completion == NULL) {
        return;
    }
    memset(completion, 0, sizeof(*completion));
    completion->pending = frame->completion != NULL;
    completion->callback = frame->completion;
    completion->link = frame->link;
    completion->result = result;
    completion->ticket = frame->ticket;
    completion->context_len = frame->completion_context_len;
    if (completion->context_len != 0) {
        memcpy(completion->context, frame->completion_context, completion->context_len);
    }
}

static void ev_mux_completion_run(const ev_mux_completion_t *completion)
{
    if ((completion != NULL) && completion->pending && (completion->callback != NULL)) {
        completion->callback(completion->result,
                             completion->link,
                             completion->context,
                             completion->context_len);
    }
}

static bool ev_mux_init_locks(void)
{
    for (size_t i = 0; i < EV_MUX_TARGET_COUNT; ++i) {
        if (s_targets[i].mutex == NULL) {
            s_targets[i].mutex = xSemaphoreCreateMutexStatic(&s_targets[i].mutex_storage);
            if (s_targets[i].mutex == NULL) {
                return false;
            }
        }
    }
    if (s_special_mutex == NULL) {
        s_special_mutex = xSemaphoreCreateMutexStatic(&s_special_mutex_storage);
    }
    if (s_special_mutex == NULL) {
        return false;
    }
    if (s_repl_producer_mutex == NULL) {
        s_repl_producer_mutex = xSemaphoreCreateMutexStatic(&s_repl_producer_mutex_storage);
    }
    if (s_repl_stage_mutex == NULL) {
        s_repl_stage_mutex = xSemaphoreCreateMutexStatic(&s_repl_stage_mutex_storage);
    }
    if ((s_repl_producer_mutex == NULL) || (s_repl_stage_mutex == NULL)) {
        return false;
    }
    for (size_t i = 0; i < EV_MUX_TICKET_COUNT; ++i) {
        if (s_tickets[i].semaphore == NULL) {
            s_tickets[i].semaphore = xSemaphoreCreateBinaryStatic(&s_tickets[i].semaphore_storage);
            if (s_tickets[i].semaphore == NULL) {
                return false;
            }
        }
    }
    return true;
}

esp_err_t ev_mux_init0(void)
{
    bool seed_init_mutex = false;
    portENTER_CRITICAL(&s_init_mutex_create_mux);
    if (s_init_mutex == NULL) {
        s_init_mutex = xSemaphoreCreateBinaryStatic(&s_init_mutex_storage);
        seed_init_mutex = s_init_mutex != NULL;
    }
    SemaphoreHandle_t init_mutex = s_init_mutex;
    portEXIT_CRITICAL(&s_init_mutex_create_mux);
    if (init_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // A static binary semaphore starts empty. Seed it only after releasing the
    // creation spinlock because queue give operations may enter RTOS critical
    // sections internally.
    if (seed_init_mutex && (xSemaphoreGive(init_mutex) != pdTRUE)) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(init_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_initialized) {
        xSemaphoreGive(init_mutex);
        return ESP_OK;
    }

    esp_err_t result = ESP_ERR_NO_MEM;
    uint8_t *arena = ev_mux_arena_alloc();
    if ((arena != NULL) && ev_mux_init_locks()) {
        s_arena_slab = arena;
        ev_mux_target_queues_bind();
        TaskHandle_t task = xTaskCreateStatic(ev_mux_tx_task,
                                              "ev_mux_tx",
                                              EV_MUX_TX_TASK_STACK_SIZE,
                                              NULL,
                                              EV_MUX_TX_TASK_PRIORITY,
                                              s_tx_task_stack,
                                              &s_tx_task_tcb);
        if (task != NULL) {
            s_tx_task = task;
            result = ESP_OK;
        }
    }

    if (result != ESP_OK) {
        s_tx_task = NULL;
        s_arena_slab = NULL;
        ev_mux_target_queues_reset();
        if (arena != NULL) {
            heap_caps_free(arena);
        }
    }

    s_initialized = (result == ESP_OK);
    xSemaphoreGive(init_mutex);
    return result;
}

static bool ev_mux_copy_options(ev_mux_frame_t *frame, const ev_mux_submit_options_t *options)
{
    if ((frame == NULL) || (options == NULL) || (options->link == NULL) ||
            (options->tx_class > EV_MUX_TX_PREVIEW) ||
            (options->completion_context_len > EV_MUX_TX_CONTEXT_MAX) ||
            ((options->completion_context_len != 0) && (options->completion_context == NULL))) {
        return false;
    }
    if (!ev_transport_link_is_current(options->link)) {
        return false;
    }
    memset(frame, 0, sizeof(*frame));
    frame->link = *options->link;
    frame->priority = (uint8_t)options->tx_class;
    frame->vm_epoch = options->vm_epoch;
    frame->completion = options->completion;
    frame->completion_context_len = (uint8_t)options->completion_context_len;
    if (options->session_id != NULL) {
        size_t session_len = strnlen(options->session_id, sizeof(frame->session_id));
        if (session_len > EV_SESSION_ID_HEX_LEN) {
            return false;
        }
        memcpy(frame->session_id, options->session_id, session_len);
    }
    if (options->vm_output_origin != NULL) {
        const ev_vm_output_origin_t *origin = options->vm_output_origin;
        size_t origin_session_len = strnlen(origin->session_id, sizeof(origin->session_id));
        if ((origin->input_generation == 0) ||
                (origin_session_len != EV_SESSION_ID_HEX_LEN) ||
                !ev_mux_link_equal(&origin->link, options->link) ||
                (origin->vm_epoch != options->vm_epoch) ||
                (options->session_id == NULL) ||
                (strcmp(origin->session_id, options->session_id) != 0)) {
            return false;
        }
        frame->output_input_generation = origin->input_generation;
        frame->output_transition_id = origin->transition_id;
    }
    if (frame->completion_context_len != 0) {
        memcpy(frame->completion_context,
               options->completion_context,
               frame->completion_context_len);
    }
    return true;
}

static bool ev_mux_build_header(ev_mux_frame_t *frame, uint32_t crc)
{
    int header_len = snprintf(frame->header,
                              sizeof(frame->header),
                              "\x1e"
                              "EVMUX/1 h=%u p=%" PRIu32 " c=%08" PRIX32 "\r\n",
                              (unsigned int)frame->metadata_len,
                              frame->payload_len,
                              crc);
    if ((header_len <= 0) || ((size_t)header_len >= sizeof(frame->header))) {
        return false;
    }
    frame->header_len = (uint16_t)header_len;
    return true;
}

static bool ev_mux_arena_copy_in(ev_mux_target_queue_t *queue,
                                 const ev_mux_frame_t *frame,
                                 const void *source,
                                 size_t len)
{
    const uint8_t *bytes = (const uint8_t *)source;
    size_t done = 0;
    while (done < len) {
        size_t block_number = done / EV_MUX_BLOCK_SIZE;
        size_t block_offset = done % EV_MUX_BLOCK_SIZE;
        size_t chunk = EV_MUX_BLOCK_SIZE - block_offset;
        if (chunk > (len - done)) {
            chunk = len - done;
        }
        uint8_t block = frame->blocks[block_number];
        memcpy(queue->arena + ((size_t)block * EV_MUX_BLOCK_SIZE) + block_offset,
               bytes + done,
               chunk);
        done += chunk;
    }
    return true;
}

static bool ev_mux_allocate_blocks(ev_mux_target_queue_t *queue,
                                   ev_mux_frame_t *frame,
                                   size_t payload_len)
{
    size_t needed = (payload_len + EV_MUX_BLOCK_SIZE - 1u) / EV_MUX_BLOCK_SIZE;
    if (needed > EV_MUX_BLOCK_COUNT) {
        return false;
    }
    frame->block_count = (uint8_t)needed;
    uint8_t found = 0;
    if (ev_mux_queue_is_tcp(queue)) {
        portENTER_CRITICAL(&s_tcp_quota_mux);
        for (uint8_t block = 0; (block < EV_MUX_BLOCK_COUNT) && (found < needed); ++block) {
            uint64_t mask = UINT64_C(1) << block;
            if ((s_tcp_free_blocks & mask) != 0) {
                s_tcp_free_blocks &= ~mask;
                frame->blocks[found++] = block;
            }
        }
        if (found != needed) {
            for (uint8_t i = 0; i < found; ++i) {
                s_tcp_free_blocks |= UINT64_C(1) << frame->blocks[i];
            }
            frame->block_count = 0;
            portEXIT_CRITICAL(&s_tcp_quota_mux);
            return false;
        }
        portEXIT_CRITICAL(&s_tcp_quota_mux);
        return true;
    }
    for (uint8_t block = 0; (block < EV_MUX_BLOCK_COUNT) && (found < needed); ++block) {
        uint64_t mask = UINT64_C(1) << block;
        if ((queue->free_blocks & mask) != 0) {
            queue->free_blocks &= ~mask;
            frame->blocks[found++] = block;
        }
    }
    if (found != needed) {
        for (uint8_t i = 0; i < found; ++i) {
            queue->free_blocks |= UINT64_C(1) << frame->blocks[i];
        }
        frame->block_count = 0;
        return false;
    }
    return true;
}

static void ev_mux_release_blocks(ev_mux_target_queue_t *queue, const ev_mux_frame_t *frame)
{
    if (ev_mux_queue_is_tcp(queue)) {
        portENTER_CRITICAL(&s_tcp_quota_mux);
        for (uint8_t i = 0; i < frame->block_count; ++i) {
            s_tcp_free_blocks |= UINT64_C(1) << frame->blocks[i];
        }
        portEXIT_CRITICAL(&s_tcp_quota_mux);
        return;
    }
    for (uint8_t i = 0; i < frame->block_count; ++i) {
        queue->free_blocks |= UINT64_C(1) << frame->blocks[i];
    }
}

static int ev_mux_free_frame_slot(ev_mux_target_queue_t *queue)
{
    for (int i = 0; i < EV_MUX_QUEUE_FRAMES; ++i) {
        if (!queue->frames[i].used) {
            return i;
        }
    }
    return -1;
}

static bool ev_mux_prepare_frame(ev_mux_frame_t *frame,
                                 const ev_mux_submit_options_t *options,
                                 const char *metadata,
                                 size_t metadata_len,
                                 size_t payload_len,
                                 size_t encoded_len,
                                 ev_mux_ticket_t ticket)
{
    if (!ev_mux_copy_options(frame, options)) {
        return false;
    }
    if ((metadata_len > EV_MUX_METADATA_MAX) || (payload_len > UINT32_MAX)) {
        return false;
    }
    memcpy(frame->metadata, metadata, metadata_len);
    frame->metadata[metadata_len] = '\0';
    frame->metadata_len = (uint16_t)metadata_len;
    frame->payload_len = (uint32_t)payload_len;
    frame->total_len = encoded_len;
    frame->ticket = ticket;
    frame->enqueue_ms = ev_mux_now_ms();
    frame->enqueue_order = __atomic_add_fetch(&s_tx_enqueue_order, 1, __ATOMIC_RELAXED);
    frame->frame_timeout_ms = ev_mux_frame_timeout(&frame->link, encoded_len);
    if (frame->frame_timeout_ms == 0) {
        return false;
    }
    frame->payload_crc_state = 0xffffffffu;
    frame->header_ready = payload_len == 0;
    if (payload_len == 0) {
        return ev_mux_build_header(frame, 0);
    }
    return ev_mux_build_header(frame, 0);
}

static void ev_mux_frame_finish_normal_locked(ev_mux_target_queue_t *queue,
                                              ev_mux_frame_t *frame,
                                              esp_err_t result,
                                              ev_mux_completion_t *completion)
{
    ev_mux_completion_prepare(frame, result, completion);
    if (frame->priority == EV_MUX_TX_PREVIEW) {
        __atomic_fetch_add(&s_tx_drop_count, 1, __ATOMIC_RELAXED);
    }
    ev_mux_release_blocks(queue, frame);
    ev_mux_tcp_release_reservation(frame);
    if (queue->bytes_used >= frame->total_len) {
        queue->bytes_used -= frame->total_len;
    } else {
        queue->bytes_used = 0;
    }
    if (queue->frames_used > 0) {
        --queue->frames_used;
    }
    memset(frame, 0, sizeof(*frame));
}

static void ev_mux_record_queue_rejection(const ev_mux_submit_options_t *options)
{
    __atomic_fetch_add(&s_tx_queue_full_count, 1, __ATOMIC_RELAXED);
    if ((options != NULL) && (options->tx_class >= EV_MUX_TX_REPL)) {
        __atomic_fetch_add(&s_tx_drop_count, 1, __ATOMIC_RELAXED);
    }
}

static esp_err_t ev_mux_enqueue_normal(const ev_mux_submit_options_t *options,
                                       const char *metadata,
                                       size_t metadata_len,
                                       const void *payload,
                                       size_t payload_len,
                                       ev_mux_ticket_t ticket,
                                       TickType_t queue_lock_wait,
                                       bool record_rejection)
{
    int target_index = ev_mux_target_index(options->link);
    if (target_index < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > EV_MUX_QUEUE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t encoded_len = EV_MUX_HEADER_MAX + metadata_len + payload_len + 1u;
    // Header length is stable for this metadata and payload length. Recompute
    // it exactly before applying the per-endpoint encoded-byte limit.
    char header[EV_MUX_HEADER_MAX];
    int header_len = snprintf(header,
                              sizeof(header),
                              "\x1e"
                              "EVMUX/1 h=%u p=%u c=%08" PRIX32 "\r\n",
                              (unsigned int)metadata_len,
                              (unsigned int)payload_len,
                              (uint32_t)0);
    if ((header_len <= 0) || ((size_t)header_len >= sizeof(header))) {
        return ESP_ERR_INVALID_SIZE;
    }
    encoded_len = (size_t)header_len + metadata_len + payload_len + 1u;
    if (encoded_len > EV_MUX_QUEUE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t frame_timeout = ev_mux_frame_timeout(options->link, encoded_len);
    if (frame_timeout == 0) {
        return ESP_ERR_TIMEOUT;
    }

    ev_mux_target_queue_t *queue = &s_targets[target_index];
    if (xSemaphoreTake(queue->mutex, queue_lock_wait) != pdTRUE) {
        if (record_rejection) {
            ev_mux_record_queue_rejection(options);
        }
        return ESP_ERR_TIMEOUT;
    }
    if (queue->frames_used >= EV_MUX_QUEUE_FRAMES) {
        if (record_rejection) {
            ev_mux_record_queue_rejection(options);
        }
        xSemaphoreGive(queue->mutex);
        return ESP_ERR_TIMEOUT;
    }
    if ((options->tx_class >= EV_MUX_TX_REPL) &&
            ((queue->frames_used >= (EV_MUX_QUEUE_FRAMES - EV_MUX_QUEUE_RESERVED_FRAMES)) ||
             ((queue->bytes_used + encoded_len) > (EV_MUX_QUEUE_BYTES - EV_MUX_QUEUE_RESERVED_BYTES)))) {
        if (record_rejection) {
            ev_mux_record_queue_rejection(options);
        }
        xSemaphoreGive(queue->mutex);
        return ESP_ERR_TIMEOUT;
    }
    if ((queue->bytes_used + encoded_len) > EV_MUX_QUEUE_BYTES) {
        if (record_rejection) {
            ev_mux_record_queue_rejection(options);
        }
        xSemaphoreGive(queue->mutex);
        return ESP_ERR_TIMEOUT;
    }
    int frame_slot = ev_mux_free_frame_slot(queue);
    if (frame_slot < 0) {
        if (record_rejection) {
            ev_mux_record_queue_rejection(options);
        }
        xSemaphoreGive(queue->mutex);
        return ESP_ERR_TIMEOUT;
    }

    ev_mux_frame_t *frame = &queue->frames[frame_slot];
    if (!ev_mux_prepare_frame(frame,
                              options,
                              metadata,
                              metadata_len,
                              payload_len,
                              encoded_len,
                              ticket)) {
        memset(frame, 0, sizeof(*frame));
        xSemaphoreGive(queue->mutex);
        return ESP_ERR_INVALID_ARG;
    }
    if (ev_mux_queue_is_tcp(queue) && !ev_mux_tcp_reserve(frame, encoded_len)) {
        if (record_rejection) {
            ev_mux_record_queue_rejection(options);
        }
        memset(frame, 0, sizeof(*frame));
        xSemaphoreGive(queue->mutex);
        return ESP_ERR_TIMEOUT;
    }
    if (!ev_mux_allocate_blocks(queue, frame, payload_len)) {
        ev_mux_tcp_release_reservation(frame);
        memset(frame, 0, sizeof(*frame));
        xSemaphoreGive(queue->mutex);
        return ESP_ERR_NO_MEM;
    }
    frame->frame_timeout_ms = frame_timeout;
    frame->used = true;
    if (payload_len != 0) {
        (void)ev_mux_arena_copy_in(queue, frame, payload, payload_len);
    }
    queue->bytes_used += encoded_len;
    ++queue->frames_used;
    xSemaphoreGive(queue->mutex);
    return ESP_OK;
}

static bool ev_mux_special_buffer_alloc(uint8_t **buffer)
{
    if (*buffer == NULL) {
        *buffer = (uint8_t *)esp_vision_memory_malloc_caps(EV_MUX_LARGE_PAYLOAD_MAX,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return *buffer != NULL;
}

static bool ev_mux_special_prepare(ev_mux_frame_t *frame,
                                   const ev_mux_submit_options_t *options,
                                   const char *metadata,
                                   size_t metadata_len,
                                   size_t payload_len,
                                   ev_mux_ticket_t ticket)
{
    size_t encoded_len = EV_MUX_HEADER_MAX + metadata_len + payload_len + 1u;
    char header[EV_MUX_HEADER_MAX];
    int header_len = snprintf(header,
                              sizeof(header),
                              "\x1e"
                              "EVMUX/1 h=%u p=%u c=%08" PRIX32 "\r\n",
                              (unsigned int)metadata_len,
                              (unsigned int)payload_len,
                              (uint32_t)0);
    if ((header_len <= 0) || ((size_t)header_len >= sizeof(header))) {
        return false;
    }
    encoded_len = (size_t)header_len + metadata_len + payload_len + 1u;
    if (!ev_mux_prepare_frame(frame,
                              options,
                              metadata,
                              metadata_len,
                              payload_len,
                              encoded_len,
                              ticket)) {
        return false;
    }
    frame->used = true;
    return true;
}

static void ev_mux_special_release_locked(ev_mux_special_frame_t *special,
                                          esp_err_t result,
                                          ev_mux_completion_t *completion)
{
    if (!special->used) {
        return;
    }
    if (special->frame.tx_offset != 0) {
        int target_index = ev_mux_target_index(&special->frame.link);
        if (target_index >= 0) {
            uintptr_t expected = (uintptr_t)special;
            (void)__atomic_compare_exchange_n(&s_special_partial_frame[target_index],
                                              &expected,
                                              (uintptr_t)0,
                                              false,
                                              __ATOMIC_RELEASE,
                                              __ATOMIC_RELAXED);
        }
    }
    ev_mux_completion_prepare(&special->frame, result, completion);
    if (special->kind == EV_MUX_SPECIAL_PREVIEW) {
        if (s_preview_pending == (int8_t)special->pool_index) {
            s_preview_pending = -1;
        }
        if (s_preview_active == (int8_t)special->pool_index) {
            s_preview_active = -1;
        }
    } else if (special->kind == EV_MUX_SPECIAL_LARGE) {
        s_large_frame.used = false;
    }
    memset(&special->frame, 0, sizeof(special->frame));
    special->used = false;
}

static esp_err_t ev_mux_submit_special(const ev_mux_submit_options_t *options,
                                       const char *metadata,
                                       const void *payload,
                                       size_t payload_len,
                                       ev_mux_special_kind_t kind,
                                       ev_mux_ticket_t ticket)
{
    if ((payload_len > EV_MUX_LARGE_PAYLOAD_MAX) || (payload == NULL && payload_len != 0)) {
        return ESP_ERR_INVALID_SIZE;
    }
    int target_index = ev_mux_target_index(options->link);
    if (target_index < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t metadata_len = strlen(metadata);
    if (metadata_len > EV_MUX_METADATA_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t encoded_len = EV_MUX_HEADER_MAX + metadata_len + payload_len + 1u;
    char header[EV_MUX_HEADER_MAX];
    int header_len = snprintf(header,
                              sizeof(header),
                              "\x1e"
                              "EVMUX/1 h=%u p=%u c=%08" PRIX32 "\r\n",
                              (unsigned int)metadata_len,
                              (unsigned int)payload_len,
                              (uint32_t)0);
    if ((header_len <= 0) || ((size_t)header_len >= sizeof(header))) {
        return ESP_ERR_INVALID_SIZE;
    }
    encoded_len = (size_t)header_len + metadata_len + payload_len + 1u;
    uint32_t frame_timeout = ev_mux_frame_timeout(options->link, encoded_len);
    if (frame_timeout == 0) {
        return ESP_ERR_TIMEOUT;
    }
    ev_mux_frame_t fresh_frame;
    if (!ev_mux_special_prepare(&fresh_frame, options, metadata, metadata_len, payload_len, ticket)) {
        return ESP_ERR_INVALID_ARG;
    }
    fresh_frame.frame_timeout_ms = frame_timeout;
    fresh_frame.priority = (kind == EV_MUX_SPECIAL_PREVIEW) ? EV_MUX_TX_PREVIEW : EV_MUX_TX_CONTROL;
    if (xSemaphoreTake(s_special_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ev_mux_special_frame_t *special = NULL;
    if (kind == EV_MUX_SPECIAL_LARGE) {
        if (s_large_frame.used) {
            xSemaphoreGive(s_special_mutex);
            return ESP_ERR_TIMEOUT;
        }
        if (!ev_mux_special_buffer_alloc(&s_large_buffer)) {
            xSemaphoreGive(s_special_mutex);
            return ESP_ERR_NO_MEM;
        }
        special = &s_large_frame;
        special->payload = s_large_buffer;
    } else {
        int8_t pool = s_preview_pending;
        if (pool < 0) {
            for (int8_t i = 0; i < 2; ++i) {
                if ((i != s_preview_active) && !s_preview_frames[i].used) {
                    pool = i;
                    break;
                }
            }
            if ((pool < 0) && (s_preview_active < 0)) {
                pool = 0;
            }
            if (pool < 0) {
                xSemaphoreGive(s_special_mutex);
                return ESP_ERR_TIMEOUT;
            }
        }
        special = &s_preview_frames[pool];
        if (special->used) {
            // A pending preview has not emitted any bytes and is replaceable.
            __atomic_fetch_add(&s_tx_drop_count, 1, __ATOMIC_RELAXED);
        }
        if (!ev_mux_special_buffer_alloc(&s_preview_buffers[pool])) {
            xSemaphoreGive(s_special_mutex);
            return ESP_ERR_NO_MEM;
        }
        special->payload = s_preview_buffers[pool];
        special->kind = EV_MUX_SPECIAL_PREVIEW;
        special->pool_index = (uint8_t)pool;
    }

    special->frame = fresh_frame;
    memcpy(special->payload, payload, payload_len);
    special->used = true;
    if (kind == EV_MUX_SPECIAL_PREVIEW) {
        s_preview_pending = (int8_t)special->pool_index;
    } else {
        special->kind = EV_MUX_SPECIAL_LARGE;
    }
    xSemaphoreGive(s_special_mutex);
    return ESP_OK;
}

static void ev_mux_ticket_rollback(ev_mux_ticket_t ticket)
{
    if (ticket != 0) {
        ev_mux_ticket_forget(ticket);
        ev_mux_ticket_complete(ticket, ESP_ERR_INVALID_STATE);
    }
}

esp_err_t ev_mux_submit_link(const ev_mux_submit_options_t *options,
                             const char *metadata,
                             const void *payload,
                             size_t payload_len,
                             ev_mux_ticket_t *ticket_out)
{
    if ((options == NULL) || (metadata == NULL) || ((payload == NULL) && (payload_len != 0))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ev_mux_init0() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    size_t metadata_len = strlen(metadata);
    if (metadata_len > EV_MUX_METADATA_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    ev_mux_ticket_t ticket = 0;
    if (ticket_out != NULL) {
        ticket = ev_mux_ticket_allocate();
        if (ticket == 0) {
            return ESP_ERR_TIMEOUT;
        }
        *ticket_out = ticket;
    }
    esp_err_t result = ev_mux_enqueue_normal(options,
                                             metadata,
                                             metadata_len,
                                             payload,
                                             payload_len,
                                             ticket,
                                             pdMS_TO_TICKS(10),
                                             true);
    if (result != ESP_OK) {
        ev_mux_ticket_rollback(ticket);
        if (ticket_out != NULL) {
            *ticket_out = 0;
        }
    }
    return result;
}

static bool ev_mux_link_equal(const ev_link_ref_t *left, const ev_link_ref_t *right)
{
    return (left->transport_id == right->transport_id) &&
           (left->slot == right->slot) &&
           (left->generation == right->generation);
}

static bool ev_mux_session_matches_output_origin(const ev_session_snapshot_t *snapshot,
                                                 const ev_link_ref_t *link,
                                                 const char *session_id,
                                                 uint32_t vm_epoch,
                                                 uint32_t input_generation,
                                                 uint32_t transition_id)
{
    return (snapshot != NULL) && (link != NULL) && (session_id != NULL) &&
           (input_generation != 0) && snapshot->has_owner &&
           (snapshot->state == EV_SESSION_OWNED) &&
           snapshot->handshake_complete && snapshot->vm_ready &&
           ev_mux_link_equal(&snapshot->owner_link, link) &&
           (strcmp(snapshot->session_id, session_id) == 0) &&
           (snapshot->vm_epoch == vm_epoch) &&
           (snapshot->input_generation == input_generation) &&
           (snapshot->transition_id == transition_id);
}

static bool ev_mux_output_origin_is_current(const ev_vm_output_origin_t *origin)
{
    if ((origin == NULL) || (origin->input_generation == 0) ||
            (strnlen(origin->session_id, sizeof(origin->session_id)) != EV_SESSION_ID_HEX_LEN)) {
        return false;
    }
    ev_session_snapshot_t snapshot;
    return ev_session_snapshot(&snapshot) &&
           ev_mux_session_matches_output_origin(&snapshot,
                                                &origin->link,
                                                origin->session_id,
                                                origin->vm_epoch,
                                                origin->input_generation,
                                                origin->transition_id);
}

typedef struct {
    char channel[EV_CONTROL_JSON_CHANNEL_CAPACITY];
    char type[EV_CONTROL_JSON_TYPE_CAPACITY];
    char method[EV_CONTROL_JSON_METHOD_MAX + 1];
    char sid[16];
    char encoding[16];
    bool has_method;
    bool has_sid;
    bool has_encoding;
} ev_mux_repl_metadata_semantics_t;

static bool ev_mux_repl_metadata_semantics(const char *text,
                                           ev_mux_repl_metadata_semantics_t *out)
{
    ev_control_json_field_t fields[] = {
        ev_control_json_string_field("channel", true, out->channel, sizeof(out->channel)),
        ev_control_json_string_field("type", true, out->type, sizeof(out->type)),
        ev_control_json_string_field("method", false, out->method, sizeof(out->method)),
        ev_control_json_string_field("sid", false, out->sid, sizeof(out->sid)),
        ev_control_json_string_field("encoding", false, out->encoding, sizeof(out->encoding)),
    };
    if (!ev_control_json_parse_object(text, strlen(text), fields,
                                      sizeof(fields) / sizeof(fields[0]))) {
        return false;
    }
    out->has_method = fields[2].present;
    out->has_sid = fields[3].present;
    out->has_encoding = fields[4].present;
    return true;
}

static bool ev_mux_repl_metadata_compatible(const char *left, const char *right)
{
    ev_mux_repl_metadata_semantics_t left_semantics = {0};
    ev_mux_repl_metadata_semantics_t right_semantics = {0};
    if (!ev_mux_repl_metadata_semantics(left, &left_semantics) ||
            !ev_mux_repl_metadata_semantics(right, &right_semantics)) {
        return false;
    }
    return (strcmp(left_semantics.channel, right_semantics.channel) == 0) &&
           (strcmp(left_semantics.type, right_semantics.type) == 0) &&
           (left_semantics.has_method == right_semantics.has_method) &&
           (!left_semantics.has_method ||
            (strcmp(left_semantics.method, right_semantics.method) == 0)) &&
           (left_semantics.has_sid == right_semantics.has_sid) &&
           (!left_semantics.has_sid || (strcmp(left_semantics.sid, right_semantics.sid) == 0)) &&
           (left_semantics.has_encoding == right_semantics.has_encoding) &&
           (!left_semantics.has_encoding ||
            (strcmp(left_semantics.encoding, right_semantics.encoding) == 0));
}

static bool ev_mux_repl_target_matches(const ev_mux_repl_stage_t *stage,
                                       const ev_mux_stream_target_t *target,
                                       const char *metadata)
{
    const char *session_id = (target->options.session_id != NULL) ? target->options.session_id : "";
    return ev_mux_link_equal(&stage->link, &target->link) &&
           (stage->vm_epoch == target->options.vm_epoch) &&
           (stage->output_input_generation == target->origin.input_generation) &&
           (stage->output_transition_id == target->origin.transition_id) &&
           (strcmp(stage->session_id, session_id) == 0) &&
           ev_mux_repl_metadata_compatible(stage->metadata, metadata);
}

static bool ev_mux_repl_stage_is_current(const ev_mux_repl_stage_t *stage)
{
    if (!ev_transport_link_is_current(&stage->link)) {
        return false;
    }
    if (stage->session_id[0] == '\0') {
        return stage->vm_epoch == 0;
    }
    if ((stage->vm_epoch != 0) &&
            (__atomic_load_n(&s_repl_cancelled_epoch, __ATOMIC_RELAXED) == stage->vm_epoch)) {
        return false;
    }
    ev_session_snapshot_t snapshot;
    if (!ev_session_snapshot(&snapshot)) {
        return false;
    }
    if (stage->output_input_generation != 0) {
        return ev_mux_session_matches_output_origin(&snapshot,
                                                    &stage->link,
                                                    stage->session_id,
                                                    stage->vm_epoch,
                                                    stage->output_input_generation,
                                                    stage->output_transition_id);
    }
    return snapshot.has_owner && (snapshot.state == EV_SESSION_OWNED) &&
           ev_mux_link_equal(&snapshot.owner_link, &stage->link) &&
           (strcmp(snapshot.session_id, stage->session_id) == 0) &&
           ((stage->vm_epoch == 0) || (snapshot.vm_epoch == stage->vm_epoch));
}

static void ev_mux_repl_stage_reset_locked(void)
{
    memset(&s_repl_stage, 0, sizeof(s_repl_stage));
}

static esp_err_t ev_mux_repl_stage_begin_locked(const ev_mux_stream_target_t *target,
                                                const char *metadata)
{
    ev_mux_repl_stage_reset_locked();
    size_t metadata_len = 0;
    esp_err_t result = ev_mux_stream_metadata_bind(metadata,
                                                   &target->options,
                                                   s_repl_stage.metadata,
                                                   &metadata_len);
    if (result != ESP_OK) {
        ev_mux_repl_stage_reset_locked();
        return result;
    }
    s_repl_stage.used = true;
    s_repl_stage.link = target->link;
    s_repl_stage.vm_epoch = target->options.vm_epoch;
    s_repl_stage.output_input_generation = target->origin.input_generation;
    s_repl_stage.output_transition_id = target->origin.transition_id;
    if (target->options.session_id != NULL) {
        memcpy(s_repl_stage.session_id,
               target->options.session_id,
               strlen(target->options.session_id) + 1);
    }
    s_repl_stage.metadata_len = (uint16_t)metadata_len;
    s_repl_stage.last_append_ms = ev_mux_now_ms();
    return ESP_OK;
}

static void ev_mux_repl_mark_stale_locked(void)
{
    ev_mux_repl_stage_reset_locked();
    __atomic_fetch_add(&s_tx_stale_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_tx_drop_count, 1, __ATOMIC_RELAXED);
}

static esp_err_t ev_mux_repl_flush_locked(uint32_t producer_started_ms,
                                          bool wait_for_space,
                                          bool *fail_session_out,
                                          ev_link_ref_t *failed_link_out,
                                          char failed_session_out[EV_SESSION_ID_HEX_LEN + 1])
{
    if (fail_session_out != NULL) {
        *fail_session_out = false;
    }
    if (xSemaphoreTake(s_repl_stage_mutex, 0) != pdTRUE) {
        __atomic_fetch_add(&s_tx_drop_count, 1, __ATOMIC_RELAXED);
        return ESP_ERR_TIMEOUT;
    }
    if (!s_repl_stage.used) {
        xSemaphoreGive(s_repl_stage_mutex);
        return ESP_OK;
    }

    ev_vm_output_origin_t output_origin = {0};
    if (s_repl_stage.output_input_generation != 0) {
        output_origin.link = s_repl_stage.link;
        memcpy(output_origin.session_id,
               s_repl_stage.session_id,
               sizeof(output_origin.session_id));
        output_origin.vm_epoch = s_repl_stage.vm_epoch;
        output_origin.input_generation = s_repl_stage.output_input_generation;
        output_origin.transition_id = s_repl_stage.output_transition_id;
    }
    ev_mux_submit_options_t options = {
        .link = &s_repl_stage.link,
        .session_id = (s_repl_stage.session_id[0] != '\0') ? s_repl_stage.session_id : NULL,
        .vm_epoch = s_repl_stage.vm_epoch,
        .tx_class = EV_MUX_TX_REPL,
        .vm_output_origin = (s_repl_stage.output_input_generation != 0) ? &output_origin : NULL,
    };
    const char *metadata = s_repl_stage.metadata;
    size_t metadata_len = s_repl_stage.metadata_len;
    const uint8_t *payload = s_repl_stage.payload;
    size_t payload_len = s_repl_stage.payload_len;
    uint32_t admission_started_ms = s_repl_stage.retrying ?
                                    s_repl_stage.retry_started_ms : producer_started_ms;
    xSemaphoreGive(s_repl_stage_mutex);
    if (!ev_mux_repl_stage_is_current(&s_repl_stage)) {
        if (xSemaphoreTake(s_repl_stage_mutex, 0) == pdTRUE) {
            ev_mux_repl_mark_stale_locked();
            xSemaphoreGive(s_repl_stage_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }

    for (;;) {
        if (!ev_mux_repl_stage_is_current(&s_repl_stage)) {
            if (xSemaphoreTake(s_repl_stage_mutex, 0) == pdTRUE) {
                if (s_repl_stage.used) {
                    ev_mux_repl_mark_stale_locked();
                }
                xSemaphoreGive(s_repl_stage_mutex);
            }
            return ESP_ERR_INVALID_STATE;
        }
        uint32_t now_ms = ev_mux_now_ms();
        if (wait_for_space &&
                ((uint32_t)(now_ms - admission_started_ms) >= EV_MUX_QUEUE_TIMEOUT_MS)) {
            ev_mux_record_queue_rejection(&options);
            if ((fail_session_out != NULL) && (options.session_id != NULL)) {
                *fail_session_out = true;
                if (failed_link_out != NULL) {
                    *failed_link_out = *options.link;
                }
                if (failed_session_out != NULL) {
                    memcpy(failed_session_out, options.session_id, sizeof(s_repl_stage.session_id));
                }
            }
            if (xSemaphoreTake(s_repl_stage_mutex, 0) == pdTRUE) {
                ev_mux_repl_stage_reset_locked();
                xSemaphoreGive(s_repl_stage_mutex);
            }
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t result = ev_mux_enqueue_normal(&options,
                                                 metadata,
                                                 metadata_len,
                                                 payload,
                                                 payload_len,
                                                 0,
                                                 0,
                                                 false);
        if (result == ESP_OK) {
            if (xSemaphoreTake(s_repl_stage_mutex, 0) == pdTRUE) {
                ev_mux_repl_stage_reset_locked();
                xSemaphoreGive(s_repl_stage_mutex);
            }
            return ESP_OK;
        }
        if (result != ESP_ERR_TIMEOUT) {
            __atomic_fetch_add(&s_tx_drop_count, 1, __ATOMIC_RELAXED);
            if ((fail_session_out != NULL) && (options.session_id != NULL)) {
                *fail_session_out = true;
                if (failed_link_out != NULL) {
                    *failed_link_out = *options.link;
                }
                if (failed_session_out != NULL) {
                    memcpy(failed_session_out, options.session_id, sizeof(s_repl_stage.session_id));
                }
            }
            if (xSemaphoreTake(s_repl_stage_mutex, 0) == pdTRUE) {
                ev_mux_repl_stage_reset_locked();
                xSemaphoreGive(s_repl_stage_mutex);
            }
            return result;
        }

        now_ms = ev_mux_now_ms();
        if (!wait_for_space) {
            if (xSemaphoreTake(s_repl_stage_mutex, 0) == pdTRUE) {
                if (!s_repl_stage.retrying) {
                    s_repl_stage.retrying = true;
                    s_repl_stage.retry_started_ms = now_ms;
                } else if ((uint32_t)(now_ms - s_repl_stage.retry_started_ms) >=
                           EV_MUX_QUEUE_TIMEOUT_MS) {
                    ev_mux_record_queue_rejection(&options);
                    if ((fail_session_out != NULL) && (options.session_id != NULL)) {
                        *fail_session_out = true;
                        if (failed_link_out != NULL) {
                            *failed_link_out = *options.link;
                        }
                        if (failed_session_out != NULL) {
                            memcpy(failed_session_out, options.session_id,
                                   sizeof(s_repl_stage.session_id));
                        }
                    }
                    ev_mux_repl_stage_reset_locked();
                }
                xSemaphoreGive(s_repl_stage_mutex);
            }
            return ESP_ERR_TIMEOUT;
        }
        if ((uint32_t)(now_ms - admission_started_ms) >= EV_MUX_QUEUE_TIMEOUT_MS) {
            ev_mux_record_queue_rejection(&options);
            if ((fail_session_out != NULL) && (options.session_id != NULL)) {
                *fail_session_out = true;
                if (failed_link_out != NULL) {
                    *failed_link_out = *options.link;
                }
                if (failed_session_out != NULL) {
                    memcpy(failed_session_out, options.session_id, sizeof(s_repl_stage.session_id));
                }
            }
            if (xSemaphoreTake(s_repl_stage_mutex, 0) == pdTRUE) {
                ev_mux_repl_stage_reset_locked();
                xSemaphoreGive(s_repl_stage_mutex);
            }
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

static void ev_mux_repl_run_failure(bool fail_session,
                                    const ev_link_ref_t *failed_link,
                                    const char *failed_session)
{
    if (fail_session && (failed_link != NULL) && (failed_session != NULL)) {
        ev_mux_fail_session(failed_link, failed_session, false);
    }
}

static esp_err_t ev_mux_write_repl_target(const ev_mux_stream_target_t *target,
                                          const char *metadata,
                                          const void *payload,
                                          size_t payload_len)
{
    if ((target == NULL) || (metadata == NULL) || ((payload == NULL) && (payload_len != 0))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len == 0) {
        return ESP_OK;
    }
    size_t metadata_len = strnlen(metadata, EV_MUX_METADATA_MAX + 1u);
    if (metadata_len > EV_MUX_METADATA_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t metadata_status = ev_mux_stream_metadata_bind(metadata,
                                                            &target->options,
                                                            NULL,
                                                            NULL);
    if (metadata_status != ESP_OK) {
        ev_mux_record_tx_drop();
        return metadata_status;
    }
    if ((target->origin.input_generation != 0) &&
            !ev_mux_output_origin_is_current(&target->origin)) {
        __atomic_fetch_add(&s_tx_stale_count, 1, __ATOMIC_RELAXED);
        ev_mux_record_tx_drop();
        return ESP_ERR_INVALID_STATE;
    }
    if (ev_mux_init0() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t producer_started_ms = ev_mux_now_ms();
    if (xSemaphoreTake(s_repl_producer_mutex, pdMS_TO_TICKS(EV_MUX_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        ev_mux_submit_options_t rejected = target->options;
        ev_mux_record_queue_rejection(&rejected);
        return ESP_ERR_TIMEOUT;
    }

    const uint8_t *bytes = (const uint8_t *)payload;
    size_t offset = 0;
    esp_err_t result = ESP_OK;
    bool fail_session = false;
    ev_link_ref_t failed_link = target->link;
    char failed_session[EV_SESSION_ID_HEX_LEN + 1] = {0};

    if (xSemaphoreTake(s_repl_stage_mutex, 0) != pdTRUE) {
        result = ESP_ERR_TIMEOUT;
        __atomic_fetch_add(&s_tx_drop_count, 1, __ATOMIC_RELAXED);
        goto repl_done;
    }
    bool stage_used = s_repl_stage.used;
    bool target_matches = !stage_used ||
                          ev_mux_repl_target_matches(&s_repl_stage, target, metadata);
    xSemaphoreGive(s_repl_stage_mutex);
    bool stage_current = !stage_used || ev_mux_repl_stage_is_current(&s_repl_stage);

    if (stage_used && !stage_current) {
        if (xSemaphoreTake(s_repl_stage_mutex, 0) == pdTRUE) {
            ev_mux_repl_mark_stale_locked();
            xSemaphoreGive(s_repl_stage_mutex);
        }
        stage_used = false;
    } else if (stage_used && !target_matches) {
        result = ev_mux_repl_flush_locked(producer_started_ms,
                                          true,
                                          &fail_session,
                                          &failed_link,
                                          failed_session);
        if (result != ESP_OK) {
            goto repl_done;
        }
    }

    while (offset < payload_len) {
        if (xSemaphoreTake(s_repl_stage_mutex, 0) != pdTRUE) {
            result = ESP_ERR_TIMEOUT;
            __atomic_fetch_add(&s_tx_drop_count, 1, __ATOMIC_RELAXED);
            break;
        }
        if (!s_repl_stage.used) {
            result = ev_mux_repl_stage_begin_locked(target, metadata);
            if (result != ESP_OK) {
                ev_mux_record_tx_drop();
                xSemaphoreGive(s_repl_stage_mutex);
                break;
            }
        }
        if (!ev_mux_repl_target_matches(&s_repl_stage, target, metadata)) {
            xSemaphoreGive(s_repl_stage_mutex);
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        size_t available = EV_MUX_REPL_STAGE_BYTES - s_repl_stage.payload_len;
        size_t remaining = payload_len - offset;
        size_t copy_len = (remaining < available) ? remaining : available;
        const uint8_t *newline = (const uint8_t *)memchr(bytes + offset, '\n', copy_len);
        if (newline != NULL) {
            copy_len = (size_t)(newline - (bytes + offset)) + 1u;
        }
        memcpy(s_repl_stage.payload + s_repl_stage.payload_len, bytes + offset, copy_len);
        s_repl_stage.payload_len += copy_len;
        s_repl_stage.last_append_ms = ev_mux_now_ms();
        bool should_flush = (s_repl_stage.payload_len == EV_MUX_REPL_STAGE_BYTES) ||
                            (s_repl_stage.payload[s_repl_stage.payload_len - 1] == '\n');
        xSemaphoreGive(s_repl_stage_mutex);
        offset += copy_len;

        if (should_flush) {
            result = ev_mux_repl_flush_locked(producer_started_ms,
                                              true,
                                              &fail_session,
                                              &failed_link,
                                              failed_session);
            if (result != ESP_OK) {
                break;
            }
        }
    }

repl_done:
    xSemaphoreGive(s_repl_producer_mutex);
    ev_mux_repl_run_failure(fail_session, &failed_link, failed_session);
    return result;
}

static void ev_mux_repl_worker_flush(void)
{
    if (xSemaphoreTake(s_repl_producer_mutex, 0) != pdTRUE) {
        return;
    }
    if (xSemaphoreTake(s_repl_stage_mutex, 0) != pdTRUE) {
        xSemaphoreGive(s_repl_producer_mutex);
        return;
    }
    uint32_t now_ms = ev_mux_now_ms();
    bool due = s_repl_stage.used &&
               ((uint32_t)(now_ms - s_repl_stage.last_append_ms) >= EV_MUX_REPL_COALESCE_MS ||
                s_repl_stage.retrying);
    xSemaphoreGive(s_repl_stage_mutex);
    if (due) {
        bool fail_session = false;
        ev_link_ref_t failed_link = {0};
        char failed_session[EV_SESSION_ID_HEX_LEN + 1] = {0};
        (void)ev_mux_repl_flush_locked(now_ms,
                                       false,
                                       &fail_session,
                                       &failed_link,
                                       failed_session);
        xSemaphoreGive(s_repl_producer_mutex);
        ev_mux_repl_run_failure(fail_session, &failed_link, failed_session);
        return;
    }
    xSemaphoreGive(s_repl_producer_mutex);
}

esp_err_t ev_mux_submit_large_link(const ev_mux_submit_options_t *options,
                                   const char *metadata,
                                   const void *payload,
                                   size_t payload_len,
                                   ev_mux_ticket_t *ticket_out)
{
    if ((options == NULL) || (metadata == NULL) ||
            ((payload == NULL) && (payload_len != 0))) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((options->session_id == NULL) || (options->session_id[0] == '\0') ||
            !ev_session_output_ready(options->link, options->session_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_len > EV_MUX_LARGE_PAYLOAD_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (ev_mux_init0() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    ev_mux_ticket_t ticket = 0;
    if (ticket_out != NULL) {
        ticket = ev_mux_ticket_allocate();
        if (ticket == 0) {
            return ESP_ERR_TIMEOUT;
        }
        *ticket_out = ticket;
    }
    esp_err_t result = ev_mux_submit_special(options,
                                             metadata,
                                             payload,
                                             payload_len,
                                             EV_MUX_SPECIAL_LARGE,
                                             ticket);
    if (result != ESP_OK) {
        ev_mux_ticket_rollback(ticket);
        if (ticket_out != NULL) {
            *ticket_out = 0;
        }
    }
    return result;
}

esp_err_t ev_mux_submit_preview_link(const ev_mux_submit_options_t *options,
                                     const char *metadata,
                                     const void *payload,
                                     size_t payload_len)
{
    if ((options == NULL) || (metadata == NULL) || (options->tx_class != EV_MUX_TX_PREVIEW)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((options->session_id == NULL) || (options->session_id[0] == '\0') ||
            !ev_session_output_ready(options->link, options->session_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ev_mux_init0() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ev_mux_submit_special(options,
                                             metadata,
                                             payload,
                                             payload_len,
                                             EV_MUX_SPECIAL_PREVIEW,
                                             0);
    if (result != ESP_OK) {
        ev_mux_record_tx_drop();
    }
    return result;
}

bool ev_mux_large_payload_available(void)
{
    ev_session_snapshot_t snapshot;
    if (!ev_session_snapshot(&snapshot) || !snapshot.has_owner ||
            !ev_session_output_ready(&snapshot.owner_link, snapshot.session_id)) {
        return false;
    }
    if ((ev_mux_init0() != ESP_OK) ||
            (xSemaphoreTake(s_special_mutex, pdMS_TO_TICKS(20)) != pdTRUE)) {
        return false;
    }
    bool available = ev_mux_special_buffer_alloc(&s_large_buffer);
    xSemaphoreGive(s_special_mutex);
    return available;
}

bool ev_mux_preview_slot_available(void)
{
    ev_session_snapshot_t snapshot;
    if (!ev_session_snapshot(&snapshot) || !snapshot.has_owner ||
            !ev_session_output_ready(&snapshot.owner_link, snapshot.session_id)) {
        return false;
    }
    if ((ev_mux_init0() != ESP_OK) ||
            (xSemaphoreTake(s_special_mutex, pdMS_TO_TICKS(20)) != pdTRUE)) {
        return false;
    }
    bool available = s_preview_pending >= 0;
    if (!available) {
        int8_t pool = (s_preview_active == 0) ? 1 : 0;
        available = ev_mux_special_buffer_alloc(&s_preview_buffers[pool]);
    }
    xSemaphoreGive(s_special_mutex);
    return available;
}

static bool ev_mux_frame_same_session(const ev_mux_frame_t *frame,
                                      const ev_link_ref_t *link,
                                      const char *session_id)
{
    return (frame->link.transport_id == link->transport_id) &&
           (frame->link.slot == link->slot) &&
           (frame->link.generation == link->generation) &&
           (strcmp(frame->session_id, session_id) == 0);
}

static bool ev_mux_frame_matches_epoch(const ev_mux_frame_t *frame, uint32_t vm_epoch)
{
    return (vm_epoch != 0) && (frame->vm_epoch == vm_epoch);
}

static size_t ev_mux_cancel_target_frames(ev_mux_target_queue_t *queue,
                                          bool by_epoch,
                                          uint32_t vm_epoch,
                                          const ev_link_ref_t *link,
                                          const char *session_id,
                                          ev_mux_completion_t completions[EV_MUX_QUEUE_FRAMES])
{
    size_t completion_count = 0;
    memset(completions, 0, sizeof(ev_mux_completion_t) * EV_MUX_QUEUE_FRAMES);
    for (size_t i = 0; i < EV_MUX_QUEUE_FRAMES; ++i) {
        ev_mux_frame_t *frame = &queue->frames[i];
        bool matches = by_epoch ? ev_mux_frame_matches_epoch(frame, vm_epoch) :
                       ((link != NULL) && (session_id != NULL) &&
                        ev_mux_frame_same_session(frame, link, session_id));
        if (frame->used && matches && (frame->tx_offset == 0)) {
            ev_mux_completion_prepare(frame, ESP_ERR_INVALID_STATE, &completions[completion_count++]);
            ev_mux_release_blocks(queue, frame);
            ev_mux_tcp_release_reservation(frame);
            queue->bytes_used -= frame->total_len;
            --queue->frames_used;
            memset(frame, 0, sizeof(*frame));
        }
    }
    return completion_count;
}

static size_t ev_mux_cancel_special_frames(bool by_epoch,
                                           uint32_t vm_epoch,
                                           const ev_link_ref_t *link,
                                           const char *session_id,
                                           ev_mux_completion_t completions[3])
{
    size_t count = 0;
    memset(completions, 0, sizeof(ev_mux_completion_t) * 3);
    for (size_t i = 0; i < 2; ++i) {
        ev_mux_special_frame_t *special = &s_preview_frames[i];
        bool matches = by_epoch ? ev_mux_frame_matches_epoch(&special->frame, vm_epoch) :
                       ((link != NULL) && (session_id != NULL) &&
                        ev_mux_frame_same_session(&special->frame, link, session_id));
        if (special->used && matches && (special->frame.tx_offset == 0)) {
            ev_mux_special_release_locked(special, ESP_ERR_INVALID_STATE, &completions[count++]);
        }
    }
    bool large_matches = by_epoch ? ev_mux_frame_matches_epoch(&s_large_frame.frame, vm_epoch) :
                         ((link != NULL) && (session_id != NULL) &&
                          ev_mux_frame_same_session(&s_large_frame.frame, link, session_id));
    if (s_large_frame.used && large_matches && (s_large_frame.frame.tx_offset == 0)) {
        ev_mux_special_release_locked(&s_large_frame, ESP_ERR_INVALID_STATE, &completions[count++]);
    }
    return count;
}

void ev_mux_cancel_vm_epoch(uint32_t vm_epoch)
{
    if ((vm_epoch == 0) || (ev_mux_init0() != ESP_OK)) {
        return;
    }
    __atomic_store_n(&s_repl_cancelled_epoch, vm_epoch, __ATOMIC_RELAXED);
    if ((xSemaphoreTake(s_repl_producer_mutex, 0) == pdTRUE)) {
        if (xSemaphoreTake(s_repl_stage_mutex, 0) == pdTRUE) {
            if (s_repl_stage.used && (s_repl_stage.vm_epoch == vm_epoch)) {
                ev_mux_repl_mark_stale_locked();
            }
            xSemaphoreGive(s_repl_stage_mutex);
        }
        xSemaphoreGive(s_repl_producer_mutex);
    }
    if (xSemaphoreTake(s_special_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    ev_mux_completion_t special_completions[3];
    size_t special_count = ev_mux_cancel_special_frames(true, vm_epoch, NULL, NULL,
                                                        special_completions);
    xSemaphoreGive(s_special_mutex);
    for (size_t i = 0; i < special_count; ++i) {
        ev_mux_completion_run(&special_completions[i]);
    }
    ev_mux_completion_t completions[EV_MUX_QUEUE_FRAMES];
    for (size_t i = 0; i < EV_MUX_TARGET_COUNT; ++i) {
        if (xSemaphoreTake(s_targets[i].mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            size_t count = ev_mux_cancel_target_frames(&s_targets[i], true, vm_epoch, NULL, NULL,
                                                       completions);
            xSemaphoreGive(s_targets[i].mutex);
            for (size_t j = 0; j < count; ++j) {
                ev_mux_completion_run(&completions[j]);
            }
        }
    }
}

void ev_mux_cancel_session(const ev_link_ref_t *link, const char *session_id)
{
    if ((link == NULL) || (session_id == NULL) || (ev_mux_init0() != ESP_OK)) {
        return;
    }
    if (xSemaphoreTake(s_special_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    ev_mux_completion_t special_completions[3];
    size_t special_count = ev_mux_cancel_special_frames(false, 0, link, session_id,
                                                        special_completions);
    xSemaphoreGive(s_special_mutex);
    for (size_t i = 0; i < special_count; ++i) {
        ev_mux_completion_run(&special_completions[i]);
    }
    ev_mux_completion_t completions[EV_MUX_QUEUE_FRAMES];
    for (size_t i = 0; i < EV_MUX_TARGET_COUNT; ++i) {
        if (xSemaphoreTake(s_targets[i].mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            size_t count = ev_mux_cancel_target_frames(&s_targets[i], false, 0, link, session_id,
                                                       completions);
            xSemaphoreGive(s_targets[i].mutex);
            for (size_t j = 0; j < count; ++j) {
                ev_mux_completion_run(&completions[j]);
            }
        }
    }
}

static ev_mux_frame_auth_t ev_mux_frame_authorized(const ev_mux_frame_t *frame)
{
    if ((frame->session_id[0] == '\0') && (frame->vm_epoch == 0)) {
        return EV_MUX_FRAME_ALLOWED;
    }
    ev_session_snapshot_t snapshot;
    if (!ev_session_snapshot(&snapshot)) {
        return EV_MUX_FRAME_STALE;
    }
    // A VM operation can finish and enqueue after reset cancellation. Reject
    // that late reply before writing; an already-started frame retains its
    // boundary and is allowed to finish within its original deadline.
    if ((frame->tx_offset == 0) && (frame->vm_epoch != 0) &&
            (frame->vm_epoch != snapshot.vm_epoch)) {
        return EV_MUX_FRAME_STALE;
    }
    if ((frame->tx_offset == 0) && (frame->output_input_generation != 0) &&
            !ev_mux_session_matches_output_origin(&snapshot,
                                                  &frame->link,
                                                  frame->session_id,
                                                  frame->vm_epoch,
                                                  frame->output_input_generation,
                                                  frame->output_transition_id)) {
        return EV_MUX_FRAME_STALE;
    }
    if (frame->session_id[0] == '\0') {
        return EV_MUX_FRAME_ALLOWED;
    }
    bool same_owner = snapshot.has_owner &&
                      (snapshot.owner_link.transport_id == frame->link.transport_id) &&
                      (snapshot.owner_link.slot == frame->link.slot) &&
                      (snapshot.owner_link.generation == frame->link.generation) &&
                      (strcmp(snapshot.session_id, frame->session_id) == 0);
    bool releasing = (snapshot.state == EV_SESSION_RELEASING) && same_owner;
    if (!same_owner && !releasing) {
        return EV_MUX_FRAME_STALE;
    }
    if (frame->priority == EV_MUX_TX_SESSION) {
        return EV_MUX_FRAME_ALLOWED;
    }
    // Once a frame has started, preserve its wire boundary for this exact
    // owner. In particular, release_prepare makes output_ready false while
    // the release ACK waits behind the partial frame. New ordinary frames
    // remain gated below; the worker still enforces this frame's deadline.
    if (frame->tx_offset != 0) {
        return EV_MUX_FRAME_ALLOWED;
    }
    if (ev_session_output_ready(&frame->link, frame->session_id)) {
        return EV_MUX_FRAME_ALLOWED;
    }
    return EV_MUX_FRAME_WAIT;
}

static const uint8_t *ev_mux_payload_pointer(const ev_mux_target_queue_t *queue,
                                             const ev_mux_special_frame_t *special,
                                             const ev_mux_frame_t *frame,
                                             size_t payload_offset,
                                             size_t *available)
{
    if (special != NULL) {
        *available = frame->payload_len - payload_offset;
        return special->payload + payload_offset;
    }
    size_t block_number = payload_offset / EV_MUX_BLOCK_SIZE;
    size_t block_offset = payload_offset % EV_MUX_BLOCK_SIZE;
    uint8_t block = frame->blocks[block_number];
    *available = EV_MUX_BLOCK_SIZE - block_offset;
    size_t remaining = frame->payload_len - payload_offset;
    if (*available > remaining) {
        *available = remaining;
    }
    return queue->arena + ((size_t)block * EV_MUX_BLOCK_SIZE) + block_offset;
}

static bool ev_mux_frame_crc_step(ev_mux_target_queue_t *queue,
                                  ev_mux_special_frame_t *special,
                                  ev_mux_frame_t *frame)
{
    if (frame->header_ready) {
        return true;
    }
    size_t stop = frame->payload_crc_pos + EV_MUX_CRC_CHUNK_BYTES;
    if (stop > frame->payload_len) {
        stop = frame->payload_len;
    }
    while (frame->payload_crc_pos < stop) {
        size_t available = 0;
        const uint8_t *data = ev_mux_payload_pointer(queue,
                                                     special,
                                                     frame,
                                                     frame->payload_crc_pos,
                                                     &available);
        size_t chunk = stop - frame->payload_crc_pos;
        if (chunk > available) {
            chunk = available;
        }
        for (size_t i = 0; i < chunk; ++i) {
            frame->payload_crc_state = ev_mux_crc32_update(frame->payload_crc_state, data[i]);
        }
        frame->payload_crc_pos += (uint32_t)chunk;
    }
    if (frame->payload_crc_pos == frame->payload_len) {
        frame->header_ready = ev_mux_build_header(frame, ~frame->payload_crc_state);
    }
    return frame->header_ready;
}

static const uint8_t *ev_mux_frame_segment(ev_mux_target_queue_t *queue,
                                           ev_mux_special_frame_t *special,
                                           ev_mux_frame_t *frame,
                                           size_t offset,
                                           size_t *segment_len)
{
    static const uint8_t eof = 0x1f;
    if (offset < frame->header_len) {
        *segment_len = frame->header_len - offset;
        return (const uint8_t *)frame->header + offset;
    }
    offset -= frame->header_len;
    if (offset < frame->metadata_len) {
        *segment_len = frame->metadata_len - offset;
        return (const uint8_t *)frame->metadata + offset;
    }
    offset -= frame->metadata_len;
    if (offset < frame->payload_len) {
        return ev_mux_payload_pointer(queue, special, frame, offset, segment_len);
    }
    *segment_len = 1;
    return &eof;
}

static int ev_mux_candidate_priority(const ev_mux_frame_t *frame)
{
    return frame->used ? (int)frame->priority : 100;
}

static bool ev_mux_choose_frame_locked(ev_mux_target_queue_t *queue,
                                       int target_index,
                                       bool special_locked,
                                       ev_mux_frame_t **out_frame,
                                       ev_mux_special_frame_t **out_special,
                                       int *out_slot)
{
    *out_frame = NULL;
    *out_special = NULL;
    *out_slot = -1;
    uint32_t now_ms = ev_mux_now_ms();
    uint32_t oldest_order = 0;
    int best_priority = 100;

    // A special frame that has started owns this target until completion.
    // Its cursor is protected by s_special_mutex, so use the atomic marker when
    // a producer currently owns that mutex instead of selecting normal output.
    if (!special_locked &&
            (__atomic_load_n(&s_special_partial_frame[target_index], __ATOMIC_ACQUIRE) != 0)) {
        return false;
    }

    // A partially emitted frame owns this endpoint until EOF or failure.
    for (int i = 0; i < EV_MUX_QUEUE_FRAMES; ++i) {
        ev_mux_frame_t *frame = &queue->frames[i];
        if (frame->used && (frame->tx_offset != 0)) {
            *out_frame = frame;
            *out_slot = i;
            return true;
        }
    }
    if (special_locked) {
        for (size_t i = 0; i < 2; ++i) {
            ev_mux_special_frame_t *special = &s_preview_frames[i];
            if (special->used && (ev_mux_target_index(&special->frame.link) == target_index) &&
                    (special->frame.tx_offset != 0)) {
                *out_frame = &special->frame;
                *out_special = special;
                return true;
            }
        }
        if (s_large_frame.used &&
                (ev_mux_target_index(&s_large_frame.frame.link) == target_index) &&
                (s_large_frame.frame.tx_offset != 0)) {
            *out_frame = &s_large_frame.frame;
            *out_special = &s_large_frame;
            return true;
        }
    }

    for (int i = 0; i < EV_MUX_QUEUE_FRAMES; ++i) {
        ev_mux_frame_t *frame = &queue->frames[i];
        if (!frame->used) {
            continue;
        }
        if ((uint32_t)(now_ms - frame->enqueue_ms) >= EV_MUX_QUEUE_TIMEOUT_MS) {
            continue;
        }
        int priority = ev_mux_candidate_priority(frame);
        if ((priority < best_priority) ||
                ((priority == best_priority) && (frame->enqueue_order < oldest_order))) {
            best_priority = priority;
            oldest_order = frame->enqueue_order;
            *out_frame = frame;
            *out_special = NULL;
            *out_slot = i;
        }
    }
    if (special_locked) {
        for (size_t i = 0; i < 2; ++i) {
            ev_mux_special_frame_t *special = &s_preview_frames[i];
            ev_mux_frame_t *frame = &special->frame;
            if (!special->used || (ev_mux_target_index(&frame->link) != target_index) ||
                    ((uint32_t)(now_ms - frame->enqueue_ms) >= EV_MUX_QUEUE_TIMEOUT_MS)) {
                continue;
            }
            int priority = ev_mux_candidate_priority(frame);
            if ((priority < best_priority) ||
                    ((priority == best_priority) && (frame->enqueue_order < oldest_order))) {
                best_priority = priority;
                oldest_order = frame->enqueue_order;
                *out_frame = frame;
                *out_special = special;
                *out_slot = -1;
            }
        }
        if (s_large_frame.used &&
                (ev_mux_target_index(&s_large_frame.frame.link) == target_index) &&
                ((uint32_t)(now_ms - s_large_frame.frame.enqueue_ms) < EV_MUX_QUEUE_TIMEOUT_MS)) {
            ev_mux_frame_t *frame = &s_large_frame.frame;
            int priority = ev_mux_candidate_priority(frame);
            if ((priority < best_priority) ||
                    ((priority == best_priority) && (frame->enqueue_order < oldest_order))) {
                *out_frame = frame;
                *out_special = &s_large_frame;
                *out_slot = -1;
            }
        }
    }
    return *out_frame != NULL;
}

static void ev_mux_drop_frame_locked(ev_mux_target_queue_t *queue,
                                     ev_mux_frame_t *frame,
                                     ev_mux_special_frame_t *special,
                                     int slot,
                                     esp_err_t result,
                                     ev_mux_completion_t *completion)
{
    if (special != NULL) {
        ev_mux_special_release_locked(special, result, completion);
    } else if ((slot >= 0) && (slot < EV_MUX_QUEUE_FRAMES)) {
        ev_mux_frame_finish_normal_locked(queue, frame, result, completion);
    }
}

static void ev_mux_fail_session(const ev_link_ref_t *link, const char *session_id, bool close_tcp)
{
    if (session_id[0] != '\0') {
        (void)ev_session_link_lost(link, EV_SESSION_CHANGE_TX_FAILED);
        ev_mux_cancel_session(link, session_id);
    }
    if (close_tcp && (link->transport_id == EV_TRANSPORT_TCP)) {
        ev_transport_link_close(link);
    }
}

static void ev_mux_process_target(size_t target_index)
{
    ev_mux_target_queue_t *queue = &s_targets[target_index];
    bool special_locked = xSemaphoreTake(s_special_mutex, 0) == pdTRUE;
    if (xSemaphoreTake(queue->mutex, 0) != pdTRUE) {
        if (special_locked) {
            xSemaphoreGive(s_special_mutex);
        }
        return;
    }

    ev_mux_frame_t *frame = NULL;
    ev_mux_special_frame_t *special = NULL;
    int slot = -1;
    if (!ev_mux_choose_frame_locked(queue, (int)target_index, special_locked,
                                    &frame, &special, &slot)) {
        // Expire any queued frame whose two-second queue deadline elapsed.
        uint32_t now_ms = ev_mux_now_ms();
        for (int i = 0; i < EV_MUX_QUEUE_FRAMES; ++i) {
            ev_mux_frame_t *expired = &queue->frames[i];
            if (expired->used && (expired->tx_offset == 0) &&
                    ((uint32_t)(now_ms - expired->enqueue_ms) >= EV_MUX_QUEUE_TIMEOUT_MS)) {
                ev_mux_completion_t completion;
                ev_mux_frame_finish_normal_locked(queue, expired, ESP_ERR_TIMEOUT, &completion);
                xSemaphoreGive(queue->mutex);
                if (special_locked) {
                    xSemaphoreGive(s_special_mutex);
                }
                ev_mux_completion_run(&completion);
                return;
            }
        }
        if (special_locked) {
            for (size_t i = 0; i < 2; ++i) {
                ev_mux_special_frame_t *expired = &s_preview_frames[i];
                if (expired->used && (ev_mux_target_index(&expired->frame.link) == (int)target_index) &&
                        (expired->frame.tx_offset == 0) &&
                        ((uint32_t)(now_ms - expired->frame.enqueue_ms) >= EV_MUX_QUEUE_TIMEOUT_MS)) {
                    ev_mux_completion_t completion;
                    ev_mux_special_release_locked(expired, ESP_ERR_TIMEOUT, &completion);
                    xSemaphoreGive(queue->mutex);
                    xSemaphoreGive(s_special_mutex);
                    ev_mux_completion_run(&completion);
                    return;
                }
            }
            if (s_large_frame.used &&
                    (ev_mux_target_index(&s_large_frame.frame.link) == (int)target_index) &&
                    (s_large_frame.frame.tx_offset == 0) &&
                    ((uint32_t)(now_ms - s_large_frame.frame.enqueue_ms) >= EV_MUX_QUEUE_TIMEOUT_MS)) {
                ev_mux_completion_t completion;
                ev_mux_special_release_locked(&s_large_frame, ESP_ERR_TIMEOUT, &completion);
                xSemaphoreGive(queue->mutex);
                xSemaphoreGive(s_special_mutex);
                ev_mux_completion_run(&completion);
                return;
            }
        }
        xSemaphoreGive(queue->mutex);
        if (special_locked) {
            xSemaphoreGive(s_special_mutex);
        }
        return;
    }

    uint32_t now_ms = ev_mux_now_ms();
    ev_mux_completion_t completion;
    memset(&completion, 0, sizeof(completion));
    bool did_finish = false;
    bool failed_partial = false;
    bool close_tcp = false;
    bool stale_drop = false;
    ev_link_ref_t failed_link = frame->link;
    char failed_session[sizeof(frame->session_id)];
    memcpy(failed_session, frame->session_id, sizeof(failed_session));

    if ((uint32_t)(now_ms - frame->enqueue_ms) >= EV_MUX_QUEUE_TIMEOUT_MS &&
            (frame->tx_offset == 0)) {
        ev_mux_drop_frame_locked(queue, frame, special, slot, ESP_ERR_TIMEOUT, &completion);
        did_finish = true;
    } else if (!ev_transport_link_is_current(&frame->link)) {
        stale_drop = true;
        failed_partial = frame->tx_offset != 0;
        __atomic_fetch_add(&s_tx_stale_count, 1, __ATOMIC_RELAXED);
        ev_mux_drop_frame_locked(queue, frame, special, slot, ESP_ERR_INVALID_STATE, &completion);
        did_finish = true;
    } else {
        ev_mux_frame_auth_t auth = ev_mux_frame_authorized(frame);
        if (auth == EV_MUX_FRAME_STALE) {
            stale_drop = true;
            failed_partial = frame->tx_offset != 0;
            __atomic_fetch_add(&s_tx_stale_count, 1, __ATOMIC_RELAXED);
            ev_mux_drop_frame_locked(queue, frame, special, slot, ESP_ERR_INVALID_STATE, &completion);
            did_finish = true;
        } else if (auth == EV_MUX_FRAME_WAIT) {
            // The acquired session's ordinary output stays queued until its
            // hello response completion flips the manager's handshake gate.
        } else if (!ev_mux_frame_crc_step(queue, special, frame)) {
            // CRC work is chunked so a large debug frame yields to other links.
        } else if (frame->attempted &&
                   ((uint32_t)(now_ms - frame->start_ms) >= frame->frame_timeout_ms)) {
            __atomic_fetch_add(&s_tx_timeout_count, 1, __ATOMIC_RELAXED);
            failed_partial = frame->tx_offset != 0;
            close_tcp = frame->link.transport_id == EV_TRANSPORT_TCP;
            ev_mux_drop_frame_locked(queue, frame, special, slot, ESP_ERR_TIMEOUT, &completion);
            did_finish = true;
        } else {
            if (!frame->attempted) {
                frame->attempted = true;
                frame->start_ms = now_ms;
            }
            size_t segment_len = 0;
            const uint8_t *segment = ev_mux_frame_segment(queue,
                                                          special,
                                                          frame,
                                                          frame->tx_offset,
                                                          &segment_len);
            if (segment_len > EV_MUX_WRITE_CHUNK_BYTES) {
                segment_len = EV_MUX_WRITE_CHUNK_BYTES;
            }
            ev_io_result_t io = ev_transport_write(&frame->link, segment, segment_len);
            if ((io.status == EV_IO_PROGRESS) && (io.transferred > 0) &&
                    (io.transferred <= segment_len)) {
                if ((special != NULL) && (special->kind == EV_MUX_SPECIAL_PREVIEW) &&
                        (frame->tx_offset == 0)) {
                    if (s_preview_pending == (int8_t)special->pool_index) {
                        s_preview_pending = -1;
                    }
                    s_preview_active = (int8_t)special->pool_index;
                }
                frame->tx_offset += io.transferred;
                if ((special != NULL) && (frame->tx_offset < frame->total_len)) {
                    int special_target = ev_mux_target_index(&frame->link);
                    if (special_target >= 0) {
                        __atomic_store_n(&s_special_partial_frame[special_target],
                                         (uintptr_t)special,
                                         __ATOMIC_RELEASE);
                    }
                }
                queue->congested = false;
                if (frame->tx_offset >= frame->total_len) {
                    ev_mux_drop_frame_locked(queue, frame, special, slot, ESP_OK, &completion);
                    did_finish = true;
                }
            } else if (io.status == EV_IO_WOULD_BLOCK) {
                // Preserve the frame cursor and let other endpoints run.
            } else {
                failed_partial = frame->tx_offset != 0;
                close_tcp = frame->link.transport_id == EV_TRANSPORT_TCP;
                queue->congested = true;
                queue->congested_at_ms = now_ms;
                ev_mux_drop_frame_locked(queue,
                                         frame,
                                         special,
                                         slot,
                                         (io.error != ESP_OK) ? io.error : ESP_FAIL,
                                         &completion);
                did_finish = true;
            }
        }
    }
    if (did_finish && (completion.result != ESP_OK)) {
        __atomic_fetch_add(&s_tx_drop_count, 1, __ATOMIC_RELAXED);
    }
    xSemaphoreGive(queue->mutex);
    if (special_locked) {
        xSemaphoreGive(s_special_mutex);
    }
    if (did_finish) {
        ev_mux_completion_run(&completion);
        if ((completion.result != ESP_OK) && !stale_drop) {
            ev_mux_fail_session(&failed_link, failed_session, close_tcp || failed_partial);
        } else if (stale_drop && failed_partial && (failed_link.transport_id == EV_TRANSPORT_TCP)) {
            ev_transport_link_close(&failed_link);
        }
    }
}

static void ev_mux_tx_worker_step(void)
{
    ev_mux_repl_worker_flush();
    for (size_t i = 0; i < EV_MUX_TARGET_COUNT; ++i) {
        ev_mux_process_target(i);
    }
}

static void ev_mux_tx_task(void *argument)
{
    (void)argument;
    for (;;) {
        ev_mux_tx_worker_step();
        vTaskDelay(1);
    }
}

static bool ev_mux_stream_target(ev_stream_t stream,
                                 ev_mux_tx_class_t tx_class,
                                 ev_mux_stream_target_t *target)
{
    if ((stream >= EV_STREAM_MAX) || (target == NULL)) {
        return false;
    }
    memset(target, 0, sizeof(*target));
    target->options.tx_class = tx_class;

    if (((tx_class == EV_MUX_TX_REPL) || (tx_class == EV_MUX_TX_PREVIEW)) &&
            ev_vm_output_scope_snapshot(&target->origin)) {
        if ((target->origin.input_generation == 0) ||
                (strnlen(target->origin.session_id,
                         sizeof(target->origin.session_id)) != EV_SESSION_ID_HEX_LEN) ||
                (ev_mux_target_index(&target->origin.link) < 0)) {
            return false;
        }
        target->link = target->origin.link;
        memcpy(target->session_id, target->origin.session_id, sizeof(target->session_id));
        target->options.link = &target->link;
        target->options.session_id = target->session_id;
        target->options.vm_epoch = target->origin.vm_epoch;
        target->options.vm_output_origin = &target->origin;
        return true;
    }

    ev_session_snapshot_t snapshot;
    if (ev_session_snapshot(&snapshot) && snapshot.has_owner) {
        target->link = snapshot.owner_link;
        memcpy(target->session_id, snapshot.session_id, sizeof(target->session_id));
        target->options.link = &target->link;
        target->options.session_id = target->session_id;
        target->options.vm_epoch = (tx_class == EV_MUX_TX_LOG) ? 0 : snapshot.vm_epoch;
        return true;
    }

    const char *sink_id = ev_channel_get(stream);
    ev_transport_id_t id;
    ev_link_ref_t link;
    if (!ev_transport_id_from_name(sink_id, &id) ||
            !ev_transport_link_ref(id, EV_TRANSPORT_PHYSICAL_SLOT, &link)) {
        return false;
    }
    target->link = link;
    target->options.link = &target->link;
    target->options.vm_epoch = 0;
    return true;
}

static esp_err_t ev_mux_stream_metadata_bind(const char *metadata,
                                             const ev_mux_submit_options_t *options,
                                             char *output,
                                             size_t *output_len)
{
    if ((metadata == NULL) || (options == NULL) || ((output == NULL) != (output_len == NULL))) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t metadata_len = strnlen(metadata, EV_MUX_METADATA_MAX + 1u);
    if (metadata_len > EV_MUX_METADATA_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    ev_control_json_metadata_t parsed;
    if (!ev_control_json_parse_metadata(metadata, metadata_len, &parsed)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool has_owner = (options->session_id != NULL) && (options->session_id[0] != '\0');
    if (!has_owner) {
        if (parsed.has_session_id || parsed.has_vm_epoch) {
            return ESP_ERR_INVALID_STATE;
        }
        if (output != NULL) {
            memcpy(output, metadata, metadata_len + 1u);
            *output_len = metadata_len;
        }
        return ESP_OK;
    }

    size_t session_len = strnlen(options->session_id, EV_SESSION_ID_HEX_LEN + 1u);
    if (session_len != EV_SESSION_ID_HEX_LEN) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < session_len; ++i) {
        char c = options->session_id[i];
        if (!(((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')))) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if ((parsed.has_session_id && (strcmp(parsed.session_id, options->session_id) != 0)) ||
            (parsed.has_vm_epoch && (parsed.vm_epoch != options->vm_epoch))) {
        return ESP_ERR_INVALID_STATE;
    }

    bool add_session = !parsed.has_session_id;
    bool add_epoch = !parsed.has_vm_epoch;
    if (!add_session && !add_epoch) {
        if (output != NULL) {
            memcpy(output, metadata, metadata_len + 1u);
            *output_len = metadata_len;
        }
        return ESP_OK;
    }

    size_t close_end = metadata_len;
    while ((close_end > 0) && ((metadata[close_end - 1] == ' ') ||
                               (metadata[close_end - 1] == '\t') ||
                               (metadata[close_end - 1] == '\r') ||
                               (metadata[close_end - 1] == '\n'))) {
        --close_end;
    }
    if ((close_end == 0) || (metadata[close_end - 1] != '}')) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t close_pos = close_end - 1u;
    char additions[96];
    int additions_len = 0;
    if (add_session) {
        additions_len = snprintf(additions,
                                 sizeof(additions),
                                 ",\"sessionId\":\"%s\"",
                                 options->session_id);
        if (additions_len <= 0 || (size_t)additions_len >= sizeof(additions)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (add_epoch) {
        int epoch_len = snprintf(additions + additions_len,
                                 sizeof(additions) - (size_t)additions_len,
                                 ",\"vmEpoch\":%" PRIu32,
                                 options->vm_epoch);
        if (epoch_len <= 0 || (size_t)epoch_len >= sizeof(additions) - (size_t)additions_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        additions_len += epoch_len;
    }
    size_t final_len = metadata_len + (size_t)additions_len;
    if (final_len > EV_MUX_METADATA_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (output == NULL) {
        return ESP_OK;
    }
    memcpy(output, metadata, close_pos);
    memcpy(output + close_pos, additions, (size_t)additions_len);
    memcpy(output + close_pos + (size_t)additions_len,
           metadata + close_pos,
           metadata_len - close_pos + 1u);
    *output_len = final_len;
    return ESP_OK;
}

esp_err_t ev_mux_write_class(ev_stream_t stream,
                             ev_mux_tx_class_t tx_class,
                             const char *metadata,
                             const void *payload,
                             size_t payload_len)
{
    ev_mux_stream_target_t target;
    if (!ev_mux_stream_target(stream, tx_class, &target)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tx_class == EV_MUX_TX_REPL) {
        return ev_mux_write_repl_target(&target, metadata, payload, payload_len);
    }
    return ev_mux_submit_link(&target.options, metadata, payload, payload_len, NULL);
}

esp_err_t ev_mux_write(ev_stream_t stream,
                       const char *metadata,
                       const void *payload,
                       size_t payload_len)
{
    return ev_mux_write_class(stream, EV_MUX_TX_CONTROL, metadata, payload, payload_len);
}

esp_err_t ev_mux_write_lossy(ev_stream_t stream,
                             const char *metadata,
                             const void *payload,
                             size_t payload_len)
{
    ev_mux_stream_target_t target;
    if (!ev_mux_stream_target(stream, EV_MUX_TX_PREVIEW, &target)) {
        ev_mux_record_tx_drop();
        return ESP_ERR_INVALID_STATE;
    }
    char bound_metadata[EV_MUX_METADATA_MAX + 1];
    size_t bound_metadata_len = 0;
    esp_err_t bind_result = ev_mux_stream_metadata_bind(metadata,
                                                        &target.options,
                                                        bound_metadata,
                                                        &bound_metadata_len);
    if (bind_result != ESP_OK) {
        ev_mux_record_tx_drop();
        return bind_result;
    }
    (void)bound_metadata_len;
    if ((target.origin.input_generation != 0) &&
            !ev_mux_output_origin_is_current(&target.origin)) {
        __atomic_fetch_add(&s_tx_stale_count, 1, __ATOMIC_RELAXED);
        ev_mux_record_tx_drop();
        return ESP_ERR_INVALID_STATE;
    }
    if (!ev_mux_preview_target_available(&target)) {
        ev_mux_record_tx_drop();
        return ESP_ERR_TIMEOUT;
    }
    return ev_mux_submit_preview_link(&target.options, bound_metadata, payload, payload_len);
}

esp_err_t ev_mux_write_sink_if_present(const char *sink_id,
                                       const char *metadata,
                                       const void *payload,
                                       size_t payload_len)
{
    ev_transport_id_t id;
    ev_link_ref_t link;
    if ((sink_id == NULL) || !ev_transport_id_from_name(sink_id, &id) ||
            !ev_transport_link_ref(id, EV_TRANSPORT_PHYSICAL_SLOT, &link)) {
        return ESP_ERR_INVALID_STATE;
    }
    ev_mux_submit_options_t options = {
        .link = &link,
        .session_id = NULL,
        .vm_epoch = 0,
        .tx_class = EV_MUX_TX_CONTROL,
    };
    return ev_mux_submit_link(&options, metadata, payload, payload_len, NULL);
}

static bool ev_mux_stream_target_congested(const ev_mux_stream_target_t *target)
{
    if (target == NULL) {
        return true;
    }
    int target_index = ev_mux_target_index(target->options.link);
    if ((target_index < 0) || (s_targets[target_index].mutex == NULL) ||
            (xSemaphoreTake(s_targets[target_index].mutex, 0) != pdTRUE)) {
        return true;
    }
    ev_mux_target_queue_t *queue = &s_targets[target_index];
    bool congested = queue->congested &&
                     ((uint32_t)(ev_mux_now_ms() - queue->congested_at_ms) <= EV_MUX_CONGESTION_COOLDOWN_MS);
    if (!congested) {
        queue->congested = false;
    }
    xSemaphoreGive(queue->mutex);
    return congested;
}

static bool ev_mux_preview_target_available(const ev_mux_stream_target_t *target)
{
    if ((target == NULL) || (target->options.session_id == NULL) ||
            (s_special_mutex == NULL) ||
            ((target->origin.input_generation != 0) &&
             !ev_mux_output_origin_is_current(&target->origin)) ||
            !ev_session_output_ready(target->options.link, target->options.session_id) ||
            ev_mux_stream_target_congested(target)) {
        return false;
    }
    if (xSemaphoreTake(s_special_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    bool available = s_preview_pending >= 0;
    if (!available) {
        int8_t pool = (s_preview_active == 0) ? 1 : 0;
        available = ev_mux_special_buffer_alloc(&s_preview_buffers[pool]);
    }
    xSemaphoreGive(s_special_mutex);
    return available;
}

bool ev_mux_stream_congested(ev_stream_t stream)
{
    ev_mux_stream_target_t target;
    if (!ev_mux_stream_target(stream, EV_MUX_TX_PREVIEW, &target)) {
        return false;
    }
    if ((target.origin.input_generation != 0) &&
            !ev_mux_output_origin_is_current(&target.origin)) {
        return true;
    }
    return ev_mux_stream_target_congested(&target);
}

bool ev_mux_preview_available(ev_stream_t stream)
{
    ev_mux_stream_target_t target;
    if (!ev_mux_stream_target(stream, EV_MUX_TX_PREVIEW, &target)) {
        return false;
    }
    return ev_mux_preview_target_available(&target);
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

uint32_t ev_mux_tx_queue_full_count(void)
{
    return __atomic_load_n(&s_tx_queue_full_count, __ATOMIC_RELAXED);
}

uint32_t ev_mux_tx_stale_count(void)
{
    return __atomic_load_n(&s_tx_stale_count, __ATOMIC_RELAXED);
}

int ev_mux_snprintf_u32(char *buf, size_t len, uint32_t value)
{
    return snprintf(buf, len, "%" PRIu32, value);
}

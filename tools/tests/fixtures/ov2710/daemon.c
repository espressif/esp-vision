/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
// A single pthread executes the FIFO timer daemon; the real upstream callback
// blocks inside its SCCB read while the caller attempts native destruction.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define CONFIG_CAMERA_OV2710_STATS_UPDATE_EN 1
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY (-1)
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define OV2710_GAIN_V(value, bit) 1
#define OV2710_REG_RED_BEFORE_GAIN_AVERAGE 1
#define OV2710_REG_GREEN_BEFORE_GAIN_AVERAGE 2
#define OV2710_REG_BLUE_BEFORE_GAIN_AVERAGE 3
#define OV2710_REG_AEC_AGC_ADJ_MSB 4
#define OV2710_REG_AEC_AGC_ADJ_LSB 5
#define OV2710_REG_AEC_EXPO_H 6
#define OV2710_REG_AEC_EXPO_M 7
#define OV2710_REG_AEC_EXPO_L 8
typedef int esp_err_t;
typedef void *TimerHandle_t;
typedef int TaskHandle_t;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int count;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef struct {
    struct {
        int red_avg, green_avg, blue_avg;
    } wb_avg;
    float agc_gain;
    unsigned aec_exp, seq;
} stats_t;
typedef struct {
    stats_t stats;
} ov2710_para_t;
typedef struct {
    void *priv;
    void *sccb_handle;
    int stream_status;
} esp_cam_sensor_device_t;
typedef void (*PendedFunction_t)(void *, uint32_t);
enum kind { READ_CALLBACK, STOP, DELETE_TIMER, ACK, EXIT_DAEMON };
struct command {
    enum kind kind;
    PendedFunction_t fn;
    void *context;
};
static struct command commands[64];
static unsigned head, tail, stop_calls, delete_calls, pend_calls, ack_executed;
static unsigned fail_stop, fail_delete, fail_pend, hold_ack;
static bool read_entered, read_release, prepare_done, timer_valid, sccb_alive;
static bool timer_stopped, timer_deleted;
static bool ack_release;
static bool direct_delete, fail_start;
static unsigned free_calls;
static int prepare_result;
static pthread_t daemon_thread;
static pthread_mutex_t state = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static esp_cam_sensor_device_t *device;

static void enqueue(struct command command)
{
    assert(tail - head < 64);
    commands[tail++ % 64] = command;
    pthread_cond_broadcast(&changed);
}
static TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return pthread_equal(pthread_self(), daemon_thread) ? 2 : 1;
}
static TaskHandle_t xTimerGetTimerDaemonTaskHandle(void)
{
    return 2;
}
static void *pvTimerGetTimerID(TimerHandle_t timer)
{
    return timer;
}
static int xTimerStop(TimerHandle_t timer, int ticks)
{
    (void)ticks; assert(timer == device && timer_valid);
    pthread_mutex_lock(&state);
    unsigned call = ++stop_calls;
    if (call == fail_stop) {
        pthread_mutex_unlock(&state);
        return 0;
    }
    enqueue((struct command) {
        STOP, NULL, NULL
    });
    pthread_mutex_unlock(&state); return pdPASS;
}
static int xTimerDelete(TimerHandle_t timer, int ticks)
{
    (void)ticks; assert(timer == device && timer_valid);
    pthread_mutex_lock(&state);
    unsigned call = ++delete_calls;
    if (call == fail_delete) {
        pthread_mutex_unlock(&state);
        return 0;
    }
    enqueue((struct command) {
        DELETE_TIMER, NULL, NULL
    });
    pthread_mutex_unlock(&state); return pdPASS;
}
static int xTimerStart(TimerHandle_t timer, int ticks)
{
    (void)ticks;
    assert(timer && timer_valid);
    return fail_start ? 0 : pdPASS;
}
static int xTimerPendFunctionCall(PendedFunction_t fn, void *context, uint32_t value, int ticks)
{
    (void)value; (void)ticks; pthread_mutex_lock(&state);
    unsigned call = ++pend_calls;
    if (call == fail_pend) {
        pthread_mutex_unlock(&state);
        return 0;
    }
    enqueue((struct command) {
        ACK, fn, context
    });
    pthread_mutex_unlock(&state); return pdPASS;
}
static void xSemaphoreGive(SemaphoreHandle_t sem)
{
    pthread_mutex_lock(&sem->lock); sem->count++;
    pthread_cond_signal(&sem->changed); pthread_mutex_unlock(&sem->lock);
}
static int xSemaphoreTake(SemaphoreHandle_t sem, int ticks)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ticks / 1000;
    deadline.tv_nsec += (ticks % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&sem->lock);
    while (!sem->count) {
        if (pthread_cond_timedwait(&sem->changed, &sem->lock, &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&sem->lock); return 0;
        }
    }
    sem->count--; pthread_mutex_unlock(&sem->lock); return pdTRUE;
}
static int ov2710_read(void *sccb, uint16_t reg, uint8_t *value)
{
    (void)sccb; (void)reg; pthread_mutex_lock(&state);
    read_entered = true; pthread_cond_broadcast(&changed);
    while (!read_release) {
        pthread_cond_wait(&changed, &state);
    }
    assert(sccb_alive && !free_calls); *value = 1;
    pthread_mutex_unlock(&state); return ESP_OK;
}
static int ov2710_write(void *sccb, uint16_t reg, uint8_t value)
{
    (void)sccb;
    (void)reg;
    (void)value;
    assert(sccb_alive);
    return ESP_OK;
}
static void observe_free(void *pointer)
{
    pthread_mutex_lock(&state);
    assert(timer_stopped && timer_deleted && pointer != NULL);
    free_calls++;
    pthread_mutex_unlock(&state);
}

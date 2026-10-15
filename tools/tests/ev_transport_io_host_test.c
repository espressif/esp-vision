/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "ev_transport.h"
#include "py/ringbuf.h"

ringbuf_t stdin_ringbuf;
ringbuf_t cdc_stdin_ringbuf;
ringbuf_t uart_stdin_ringbuf;

static pthread_mutex_t s_test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_test_cond = PTHREAD_COND_INITIALIZER;
static bool s_driver_entered;
static bool s_allow_driver_return;
static bool s_mutation_started;
static bool s_mutation_done;
static bool s_driver_returned_after_mutation;
static unsigned s_semaphore_contentions;

void ev_transport_test_semaphore_contended(void)
{
    pthread_mutex_lock(&s_test_mutex);
    ++s_semaphore_contentions;
    pthread_cond_broadcast(&s_test_cond);
    pthread_mutex_unlock(&s_test_mutex);
}

int ringbuf_get(ringbuf_t *ringbuf)
{
    (void)ringbuf;
    return -1;
}

size_t usb_serial_jtag_tx_some(const char *str, size_t len)
{
    (void)str;
    pthread_mutex_lock(&s_test_mutex);
    s_driver_entered = true;
    pthread_cond_broadcast(&s_test_cond);
    while (!s_allow_driver_return) {
        pthread_cond_wait(&s_test_cond, &s_test_mutex);
    }
    s_driver_returned_after_mutation = s_mutation_done;
    pthread_mutex_unlock(&s_test_mutex);
    return len == 0 ? 0 : 1;
}

typedef struct {
    ev_link_ref_t link;
    ev_io_result_t result;
} writer_args_t;

static void *writer_thread(void *arg)
{
    writer_args_t *args = arg;
    static const char byte = 'x';
    args->result = ev_transport_write(&args->link, &byte, sizeof(byte));
    return NULL;
}

typedef enum {
    MUTATE_CLOSE,
    MUTATE_ADVANCE,
} mutation_kind_t;

typedef struct {
    mutation_kind_t kind;
    ev_link_ref_t old_link;
    ev_link_ref_t new_link;
} mutation_args_t;

static void *mutation_thread(void *arg)
{
    mutation_args_t *args = arg;
    pthread_mutex_lock(&s_test_mutex);
    s_mutation_started = true;
    pthread_cond_broadcast(&s_test_cond);
    pthread_mutex_unlock(&s_test_mutex);

    if (args->kind == MUTATE_CLOSE) {
        ev_transport_link_close(&args->old_link);
    } else {
        assert(ev_transport_link_advance_generation(&args->old_link, &args->new_link));
    }

    pthread_mutex_lock(&s_test_mutex);
    s_mutation_done = true;
    pthread_cond_broadcast(&s_test_cond);
    pthread_mutex_unlock(&s_test_mutex);
    return NULL;
}

static struct timespec deadline_after_seconds(time_t seconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;
    return deadline;
}

static bool wait_for_driver_or_contention(void)
{
    struct timespec deadline = deadline_after_seconds(5);
    pthread_mutex_lock(&s_test_mutex);
    int status = 0;
    while (!s_driver_entered && (status == 0)) {
        status = pthread_cond_timedwait(&s_test_cond, &s_test_mutex, &deadline);
    }
    assert(s_driver_entered);
    while ((s_semaphore_contentions == 0) && !s_mutation_done && (status == 0)) {
        status = pthread_cond_timedwait(&s_test_cond, &s_test_mutex, &deadline);
    }
    bool reached_gate = s_semaphore_contentions != 0;
    pthread_mutex_unlock(&s_test_mutex);
    return reached_gate;
}

static void run_paused_write_mutation(mutation_kind_t kind)
{
    pthread_mutex_lock(&s_test_mutex);
    s_driver_entered = false;
    s_allow_driver_return = false;
    s_mutation_started = false;
    s_mutation_done = false;
    s_driver_returned_after_mutation = false;
    s_semaphore_contentions = 0;
    pthread_mutex_unlock(&s_test_mutex);

    ev_link_ref_t ref;
    assert(ev_transport_link_ref(EV_TRANSPORT_USJ, 0, &ref));
    writer_args_t writer = { .link = ref };
    mutation_args_t mutation = { .kind = kind, .old_link = ref };
    pthread_t writer_id;
    pthread_t mutation_id;
    assert(pthread_create(&writer_id, NULL, writer_thread, &writer) == 0);

    pthread_mutex_lock(&s_test_mutex);
    struct timespec deadline = deadline_after_seconds(5);
    int status = 0;
    while (!s_driver_entered && (status == 0)) {
        status = pthread_cond_timedwait(&s_test_cond, &s_test_mutex, &deadline);
    }
    bool driver_paused = s_driver_entered;
    pthread_mutex_unlock(&s_test_mutex);
    assert(driver_paused);

    assert(pthread_create(&mutation_id, NULL, mutation_thread, &mutation) == 0);
    bool waited_for_gate = wait_for_driver_or_contention();

    pthread_mutex_lock(&s_test_mutex);
    bool mutation_finished_while_paused = s_mutation_done;
    s_allow_driver_return = true;
    pthread_cond_broadcast(&s_test_cond);
    pthread_mutex_unlock(&s_test_mutex);

    assert(pthread_join(writer_id, NULL) == 0);
    assert(pthread_join(mutation_id, NULL) == 0);
    assert(waited_for_gate);
    assert(!mutation_finished_while_paused);
    assert(!s_driver_returned_after_mutation);
    assert(writer.result.status == EV_IO_PROGRESS);
    assert(writer.result.transferred == 1);
    assert(!ev_transport_link_is_current(&ref));
    if (kind == MUTATE_ADVANCE) {
        assert(ev_transport_link_is_current(&mutation.new_link));
        assert(mutation.new_link.generation != ref.generation);
    }
}

int main(void)
{
    assert(ev_transport_init0() == ESP_OK);
    run_paused_write_mutation(MUTATE_CLOSE);

    ev_link_ref_t reopened;
    assert(ev_transport_link_open(EV_TRANSPORT_USJ, 0, &reopened) == ESP_OK);
    run_paused_write_mutation(MUTATE_ADVANCE);
    puts("serial write generation gate passed");
    return 0;
}

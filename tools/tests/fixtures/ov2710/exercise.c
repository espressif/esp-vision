/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
static void *daemon_main(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&state);
        while (head == tail) {
            pthread_cond_wait(&changed, &state);
        }
        struct command command = commands[head++ % 64];
        pthread_mutex_unlock(&state);
        switch (command.kind) {
        case READ_CALLBACK: wb_timer_callback(device); break;
        case STOP:
            pthread_mutex_lock(&state); timer_stopped = true; pthread_mutex_unlock(&state); break;
        case DELETE_TIMER:
            pthread_mutex_lock(&state); assert(timer_stopped); timer_valid = false; timer_deleted = true;
            pthread_mutex_unlock(&state); break;
        case ACK:
            pthread_mutex_lock(&state);
            if (++ack_executed == hold_ack) {
                while (!ack_release) {
                    pthread_cond_wait(&changed, &state);
                }
            }
            assert(!free_calls);
            pthread_mutex_unlock(&state); command.fn(command.context, 0); break;
        case EXIT_DAEMON: return NULL;
        }
    }
}
static void *prepare_main(void *unused)
{
    (void)unused;
    int result = direct_delete ? ov2710_delete(device) : esp_vision_ov2710_prepare_delete(device);
    pthread_mutex_lock(&state); prepare_result = result; prepare_done = true;
    pthread_cond_broadcast(&changed); pthread_mutex_unlock(&state); return NULL;
}
static void prepare_from_daemon(void *unused, uint32_t value)
{
    (void)unused; (void)value;
    int result = esp_vision_ov2710_prepare_delete(device);
    assert(result == ESP_ERR_INVALID_STATE);
    pthread_mutex_lock(&state); prepare_done = true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&state);
}
static void run_case(unsigned scenario)
{
    head = tail = stop_calls = delete_calls = pend_calls = ack_executed = free_calls = 0;
    fail_stop = fail_delete = fail_pend = hold_ack = 0;
    read_entered = read_release = prepare_done = timer_stopped = timer_deleted = ack_release = false;
    timer_valid = sccb_alive = true;
    direct_delete = scenario >= 8; fail_start = false;
    device = calloc(1, sizeof(*device));
    struct ov2710_cam *cam = calloc(1, sizeof(*cam)); device->priv = cam; cam->wb_timer_handle = device;
    pthread_mutex_init(&cam->wb_barrier_storage.lock, NULL);
    pthread_cond_init(&cam->wb_barrier_storage.changed, NULL);
    cam->wb_barrier = &cam->wb_barrier_storage;
    assert(pthread_create(&daemon_thread, NULL, daemon_main, NULL) == 0);
    if (scenario == 0) {
        fail_start = true; assert(ov2710_set_stream(device, 1) == ESP_ERR_TIMEOUT && !device->stream_status);
        fail_start = false; device->stream_status = 1; fail_stop = 1;
        assert(ov2710_set_stream(device, 0) == ESP_ERR_TIMEOUT && device->stream_status == 1);
        fail_stop = stop_calls = 0;
    }
    if (scenario == 7) {
        pthread_mutex_lock(&state); enqueue((struct command) {
            ACK, prepare_from_daemon, NULL
        });
        while (!prepare_done) {
            pthread_cond_wait(&changed, &state);
        }
        pthread_mutex_unlock(&state);
        assert(!cam->wb_delete_started && !stop_calls && !delete_calls);
    } else {
        if (scenario == 1) {
            fail_stop = 1;
        }
        if (scenario == 2) {
            fail_pend = 1;
        }
        if (scenario == 3) {
            fail_delete = 1;
        }
        if (scenario == 4) {
            fail_pend = 2;
        }
        if (scenario == 6) {
            hold_ack = 2;
        }
        if (scenario == 9) {
            fail_pend = 1;
        }
        pthread_mutex_lock(&state); enqueue((struct command) {
            READ_CALLBACK, NULL, NULL
        });
        while (!read_entered) {
            pthread_cond_wait(&changed, &state);
        }
        pthread_mutex_unlock(&state);
        pthread_t caller; assert(pthread_create(&caller, NULL, prepare_main, NULL) == 0);
        pthread_mutex_lock(&state);
        if (scenario == 0 || scenario == 8) {
            while (!stop_calls) {
                pthread_cond_wait(&changed, &state);
            }
            assert(!prepare_done && !free_calls && sccb_alive && !timer_stopped);
        } else if (scenario == 1 || scenario == 2 || scenario == 5 || scenario == 9) {
            // Scenario 5 times out for real while the SCCB callback remains
            // blocked; its pending acknowledgement retains the heap context.
            while (!prepare_done) {
                pthread_cond_wait(&changed, &state);
            }
            assert(prepare_result == ESP_ERR_TIMEOUT && !free_calls && sccb_alive);
        }
        read_release = true; pthread_cond_broadcast(&changed);
        while (!prepare_done) {
            pthread_cond_wait(&changed, &state);
        }
        pthread_mutex_unlock(&state); pthread_join(caller, NULL);
        if (direct_delete) {
            if (scenario == 9) {
                assert(prepare_result == ESP_ERR_TIMEOUT && free_calls == 0 && device->priv == cam);
                assert(ov2710_delete(device) == ESP_OK);
            } else {
                assert(prepare_result == ESP_OK);
            }
            assert(free_calls == 2 && device->priv == NULL && timer_deleted && timer_stopped);
            goto finish;
        }
        assert(cam->wb_delete_started);
        assert(ov2710_set_stream(device, 1) == ESP_ERR_INVALID_STATE);
        if (scenario) {
            assert(prepare_result == ESP_ERR_TIMEOUT);
            if (scenario == 4 || scenario == 6) {
                assert(cam->wb_timer_handle == NULL);
            }
            if (scenario == 5 || scenario == 6) {
                assert(cam->wb_barrier_pending);
            }
            pthread_mutex_lock(&state); ack_release = true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&state);
        } else {
            assert(prepare_result == ESP_OK);
        }
    }
    assert(esp_vision_ov2710_prepare_delete(device) == ESP_OK);
    assert(cam->wb_delete_complete && !cam->wb_barrier_pending && cam->wb_timer_handle == NULL);
    assert(timer_deleted && timer_stopped);
    unsigned deletes = delete_calls, commands_before = tail;
    assert(esp_vision_ov2710_prepare_delete(device) == ESP_OK && commands_before == tail);
    // Post-preflight STREAMOFF still writes registers, but cannot stop NULL.
    assert(ov2710_set_stream(device, 0) == ESP_OK);
    assert(ov2710_delete(device) == ESP_OK && free_calls == 2 && deletes == delete_calls);
finish:
    // Production Board Manager may release SCCB only after this preparation.
    sccb_alive = false;
    pthread_mutex_lock(&state); enqueue((struct command) {
        EXIT_DAEMON, NULL, NULL
    }); pthread_mutex_unlock(&state);
    pthread_join(daemon_thread, NULL);
    pthread_mutex_destroy(&cam->wb_barrier_storage.lock); pthread_cond_destroy(&cam->wb_barrier_storage.changed);
    free(cam); free(device);
}
int main(void)
{
    for (unsigned scenario = 0; scenario < 10; scenario++) {
        run_case(scenario);
    }
    puts("OV2710 FIFO callback, queue failures, real late acknowledgements and TimerTask rejection passed");
    return 0;
}

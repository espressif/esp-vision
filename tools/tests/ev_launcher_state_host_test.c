/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ev_launcher_state.h"

int ev_test_in_critical;
static uint32_t test_vm_epoch = 7;
static uint32_t random_value = 1;

uint32_t esp_random(void)
{
    return random_value++;
}

int esp_efuse_mac_get_default(uint8_t mac[6])
{
    const uint8_t value[6] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xab };
    memcpy(mac, value, sizeof(value));
    return 0;
}

uint32_t ev_session_vm_epoch(void)
{
    // The production accessor takes a mutex. The launcher must call it before
    // entering its FreeRTOS critical section to avoid lock inversion/deadlock.
    assert(!ev_test_in_critical);
    return test_vm_epoch;
}

int main(void)
{
    static const char hash[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char active[] = "/apps/demo/revisions/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/demo.py";
    static const char op_a[] = "00000000-0000-4000-8000-000000000001";
    static const char op_b[] = "00000000-0000-4000-8000-000000000002";
    static const char op_c[] = "00000000-0000-4000-8000-000000000003";
    static const char op_d[] = "00000000-0000-4000-8000-000000000004";
    static const char runner_a[] = "/.esp-vision/staging/00000000-0000-4000-8000-000000000001/runner.py";
    static const char source_a[] = "/.esp-vision/staging/00000000-0000-4000-8000-000000000001/candidate.py";
    static const char runner_b[] = "/sdcard/.esp-vision/staging/00000000-0000-4000-8000-000000000002/runner.py";
    static const char source_b[] = "/sdcard/.esp-vision/staging/00000000-0000-4000-8000-000000000002/candidate.py";
    static const char runner_c[] = "/.esp-vision/staging/00000000-0000-4000-8000-000000000003/runner.py";
    static const char source_c[] = "/.esp-vision/staging/00000000-0000-4000-8000-000000000003/candidate.py";
    static const char runner_d[] = "/.esp-vision/staging/00000000-0000-4000-8000-000000000004/runner.py";
    static const char source_d[] = "/.esp-vision/staging/00000000-0000-4000-8000-000000000004/candidate.py";
    ev_launcher_state_snapshot_t before, after;

    esp_vision_launcher_state_init0();
    assert(esp_vision_launcher_state_snapshot(&before));
    assert(before.device_id_valid);
    assert(strcmp(before.device_id, "1032547698ab") == 0);

    // Only the firmware's one-shot main.py window may select active source.
    assert(!esp_vision_launcher_begin_active(1, hash, active));
    esp_vision_launcher_main_entry_begin();
    assert(esp_vision_launcher_begin_active(1, hash, active));
    assert(!esp_vision_launcher_begin_active(1, hash, active));
    assert(esp_vision_launcher_finish_active(EV_LAUNCHER_OUTCOME_NORMAL_RETURN));
    esp_vision_launcher_main_entry_end();

    // A finished trial operation cannot replay, but a new authorized operation
    // can run after a VM reset. The reset advances VM identity, not hard boot ID.
    assert(esp_vision_launcher_arm_trial(op_a, runner_a, source_a, hash));
    assert(esp_vision_launcher_begin_trial(op_a, source_a, hash));
    assert(!esp_vision_launcher_arm_trial(op_a, runner_a, source_a, hash));
    assert(esp_vision_launcher_finish_trial(EV_LAUNCHER_OUTCOME_NORMAL_RETURN));
    test_vm_epoch++;
    esp_vision_launcher_state_vm_reset_begin();
    assert(esp_vision_launcher_state_snapshot(&after));
    assert(strcmp(after.boot_id, before.boot_id) == 0);
    assert(after.vm_epoch == test_vm_epoch);
    assert(!after.selection_valid);
    assert(after.trial_status == EV_LAUNCHER_TRIAL_FINISHED);
    assert(!esp_vision_launcher_arm_trial(op_a, runner_a, source_a, hash));
    assert(esp_vision_launcher_arm_trial(op_b, runner_b, source_b, hash));
    assert(esp_vision_launcher_begin_trial(op_b, source_b, hash));
    test_vm_epoch++;
    esp_vision_launcher_state_vm_reset_begin();
    assert(esp_vision_launcher_state_snapshot(&after));
    assert(strcmp(after.boot_id, before.boot_id) == 0);
    assert(after.trial_status == EV_LAUNCHER_TRIAL_FINISHED);
    assert(after.trial_outcome == EV_LAUNCHER_OUTCOME_RESET);
    assert(!esp_vision_launcher_arm_trial(op_b, runner_b, source_b, hash));

    assert(esp_vision_launcher_arm_trial(op_c, runner_c, source_c, hash));
    assert(esp_vision_launcher_cancel_trial_arm(op_c));
    assert(!esp_vision_launcher_cancel_trial_arm(op_c));
    assert(!esp_vision_launcher_arm_trial(op_c, runner_c, source_c, hash));
    assert(esp_vision_launcher_arm_trial(op_d, runner_d, source_d, hash));

    // A fresh main.py entry gets one selection attempt; manual REPL calls stay
    // denied after VM reset and the guard is consumed by one authorized call.
    assert(!esp_vision_launcher_begin_active(2, hash, active));
    esp_vision_launcher_main_entry_begin();
    assert(esp_vision_launcher_begin_active(2, hash, active));
    esp_vision_launcher_main_entry_end();
    assert(!esp_vision_launcher_begin_active(2, hash, active));
    assert(!ev_test_in_critical);
    return 0;
}

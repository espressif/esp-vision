/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define EV_LAUNCHER_BOOT_ID_HEX_LEN (32)
#define EV_LAUNCHER_DEVICE_ID_HEX_LEN (12)
#define EV_LAUNCHER_OPERATION_ID_LEN (36)
#define EV_LAUNCHER_SOURCE_HASH_HEX_LEN (64)
#define EV_LAUNCHER_SOURCE_PATH_MAX (512)

typedef enum {
    EV_LAUNCHER_OUTCOME_UNKNOWN = 0,
    EV_LAUNCHER_OUTCOME_RUNNING,
    EV_LAUNCHER_OUTCOME_NORMAL_RETURN,
    EV_LAUNCHER_OUTCOME_UNCAUGHT_EXCEPTION,
    EV_LAUNCHER_OUTCOME_USER_INTERRUPTION,
    EV_LAUNCHER_OUTCOME_RESET,
} ev_launcher_outcome_t;

typedef enum {
    EV_LAUNCHER_TRIAL_NOT_STARTED = 0,
    EV_LAUNCHER_TRIAL_ARMED,
    EV_LAUNCHER_TRIAL_RUNNING,
    EV_LAUNCHER_TRIAL_FINISHED,
} ev_launcher_trial_status_t;

typedef struct {
    char boot_id[EV_LAUNCHER_BOOT_ID_HEX_LEN + 1];
    char device_id[EV_LAUNCHER_DEVICE_ID_HEX_LEN + 1];
    bool device_id_valid;
    uint32_t vm_epoch;
    bool selection_valid;
    uint64_t generation;
    char source_hash[EV_LAUNCHER_SOURCE_HASH_HEX_LEN + 1];
    char source_path[EV_LAUNCHER_SOURCE_PATH_MAX + 1];
    ev_launcher_outcome_t execution_outcome;
    bool trial_valid;
    char operation_id[EV_LAUNCHER_OPERATION_ID_LEN + 1];
    char last_trial_operation_id[EV_LAUNCHER_OPERATION_ID_LEN + 1];
    char trial_source_hash[EV_LAUNCHER_SOURCE_HASH_HEX_LEN + 1];
    char trial_source_path[EV_LAUNCHER_SOURCE_PATH_MAX + 1];
    uint32_t trial_vm_epoch;
    ev_launcher_trial_status_t trial_status;
    ev_launcher_outcome_t trial_outcome;
} ev_launcher_state_snapshot_t;

void esp_vision_launcher_state_init0(void);
void esp_vision_launcher_state_vm_reset_begin(void);
void esp_vision_launcher_main_entry_begin(void);
void esp_vision_launcher_main_entry_end(void);
bool esp_vision_launcher_begin_active(uint64_t generation, const char *source_hash,
                                      const char *source_path);
bool esp_vision_launcher_finish_active(ev_launcher_outcome_t outcome);
bool esp_vision_launcher_arm_trial(const char *operation_id, const char *runner_path,
                                   const char *source_path, const char *source_hash);
bool esp_vision_launcher_begin_trial(const char *operation_id, const char *source_path,
                                     const char *source_hash);
bool esp_vision_launcher_finish_trial(ev_launcher_outcome_t outcome);
bool esp_vision_launcher_cancel_trial_arm(const char *operation_id);
bool esp_vision_launcher_valid_operation_id(const char *operation_id);
bool esp_vision_launcher_state_snapshot(ev_launcher_state_snapshot_t *out);
bool esp_vision_launcher_trial_snapshot(const char *operation_id,
                                        ev_launcher_state_snapshot_t *out);
const char *esp_vision_launcher_outcome_name(ev_launcher_outcome_t outcome);
const char *esp_vision_launcher_trial_status_name(ev_launcher_trial_status_t status);

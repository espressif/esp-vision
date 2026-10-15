/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ev_launcher_state.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "ev_session.h"

static portMUX_TYPE s_launcher_mux = portMUX_INITIALIZER_UNLOCKED;
static ev_launcher_state_snapshot_t s_launcher_state;
static bool s_initialized;
static bool s_main_entry_window;
static bool s_main_entry_consumed;

static bool ev_launcher_hex(const char *value, size_t length)
{
    if (value == NULL || strlen(value) != length) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
                (value[i] >= 'a' && value[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool ev_launcher_slug(const char *start, size_t length)
{
    if (length == 0 || length > 48 || start[0] < 'a' || start[0] > 'z') {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const char c = start[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

static bool ev_launcher_operation_id(const char *value)
{
    if (value == NULL || strlen(value) != EV_LAUNCHER_OPERATION_ID_LEN) {
        return false;
    }
    for (size_t i = 0; i < EV_LAUNCHER_OPERATION_ID_LEN; ++i) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return value[14] == '4' && (value[19] == '8' || value[19] == '9' ||
                                value[19] == 'a' || value[19] == 'b');
}

bool esp_vision_launcher_valid_operation_id(const char *operation_id)
{
    return ev_launcher_operation_id(operation_id);
}

static bool ev_launcher_active_path(const char *path, const char *hash)
{
    if (path == NULL || strlen(path) > EV_LAUNCHER_SOURCE_PATH_MAX ||
            !ev_launcher_hex(hash, EV_LAUNCHER_SOURCE_HASH_HEX_LEN) ||
            strstr(path, "..") != NULL || strstr(path, "//") != NULL || strchr(path, '\\') != NULL) {
        return false;
    }
    const char *app = NULL;
    if (strncmp(path, "/apps/", 6) == 0) {
        app = path + 6;
    } else if (strncmp(path, "/sdcard/apps/", 13) == 0) {
        app = path + 13;
    } else {
        return false;
    }

    const char *revisions = strstr(app, "/revisions/");
    if (revisions == NULL || !ev_launcher_slug(app, (size_t)(revisions - app))) {
        return false;
    }
    const char *digest = revisions + strlen("/revisions/");
    if (strlen(digest) != EV_LAUNCHER_SOURCE_HASH_HEX_LEN + 1 + (size_t)(revisions - app) + 3 ||
            memcmp(digest, hash, EV_LAUNCHER_SOURCE_HASH_HEX_LEN) != 0 ||
            digest[EV_LAUNCHER_SOURCE_HASH_HEX_LEN] != '/') {
        return false;
    }
    const char *filename = digest + EV_LAUNCHER_SOURCE_HASH_HEX_LEN + 1;
    const size_t slug_len = (size_t)(revisions - app);
    return memcmp(filename, app, slug_len) == 0 && strcmp(filename + slug_len, ".py") == 0;
}

static bool ev_launcher_trial_paths(const char *operation_id, const char *runner_path,
                                    const char *source_path, const char *source_hash)
{
    if (!ev_launcher_operation_id(operation_id) || runner_path == NULL || source_path == NULL ||
            strlen(runner_path) > EV_LAUNCHER_SOURCE_PATH_MAX ||
            strlen(source_path) > EV_LAUNCHER_SOURCE_PATH_MAX ||
            !ev_launcher_hex(source_hash, EV_LAUNCHER_SOURCE_HASH_HEX_LEN)) {
        return false;
    }

    char flash_root[64];
    char sd_root[72];
    (void)snprintf(flash_root, sizeof(flash_root), "/.esp-vision/staging/%s/", operation_id);
    (void)snprintf(sd_root, sizeof(sd_root), "/sdcard/.esp-vision/staging/%s/", operation_id);
    const char *root = strncmp(runner_path, flash_root, strlen(flash_root)) == 0 ? flash_root :
                       strncmp(runner_path, sd_root, strlen(sd_root)) == 0 ? sd_root : NULL;
    if (root == NULL || strcmp(runner_path + strlen(root), "runner.py") != 0 ||
            strncmp(source_path, root, strlen(root)) != 0) {
        return false;
    }
    // The source path is exactly the sibling candidate file. Do not accept a
    // caller-selected path under the staging tree.
    return strcmp(source_path + strlen(root), "candidate.py") == 0;
}

static void ev_launcher_new_boot_id(char *out, size_t out_size)
{
    const uint32_t words[4] = { esp_random(), esp_random(), esp_random(), esp_random() };
    (void)snprintf(out, out_size, "%08x%08x%08x%08x", (unsigned int)words[0],
                   (unsigned int)words[1], (unsigned int)words[2], (unsigned int)words[3]);
    if (!ev_launcher_hex(out, EV_LAUNCHER_BOOT_ID_HEX_LEN)) {
        memcpy(out, "00000000000000000000000000000001", EV_LAUNCHER_BOOT_ID_HEX_LEN + 1);
    }
}

void esp_vision_launcher_state_init0(void)
{
    if (s_initialized) {
        return;
    }
    char boot_id[EV_LAUNCHER_BOOT_ID_HEX_LEN + 1];
    char device_id[EV_LAUNCHER_DEVICE_ID_HEX_LEN + 1] = { 0 };
    uint8_t mac[6] = { 0 };
    const bool device_id_valid = esp_efuse_mac_get_default(mac) == ESP_OK;
    if (device_id_valid) {
        (void)snprintf(device_id, sizeof(device_id), "%02x%02x%02x%02x%02x%02x",
                       (unsigned int)mac[0], (unsigned int)mac[1], (unsigned int)mac[2],
                       (unsigned int)mac[3], (unsigned int)mac[4], (unsigned int)mac[5]);
    }
    ev_launcher_new_boot_id(boot_id, sizeof(boot_id));
    portENTER_CRITICAL(&s_launcher_mux);
    if (!s_initialized) {
        memset(&s_launcher_state, 0, sizeof(s_launcher_state));
        memcpy(s_launcher_state.boot_id, boot_id, sizeof(boot_id));
        memcpy(s_launcher_state.device_id, device_id, sizeof(device_id));
        s_launcher_state.device_id_valid = device_id_valid && ev_launcher_hex(device_id, EV_LAUNCHER_DEVICE_ID_HEX_LEN);
        s_launcher_state.execution_outcome = EV_LAUNCHER_OUTCOME_UNKNOWN;
        s_launcher_state.trial_status = EV_LAUNCHER_TRIAL_NOT_STARTED;
        s_launcher_state.trial_outcome = EV_LAUNCHER_OUTCOME_UNKNOWN;
        s_initialized = true;
    }
    portEXIT_CRITICAL(&s_launcher_mux);
}

void esp_vision_launcher_state_vm_reset_begin(void)
{
    portENTER_CRITICAL(&s_launcher_mux);
    if (s_launcher_state.execution_outcome == EV_LAUNCHER_OUTCOME_RUNNING) {
        s_launcher_state.execution_outcome = EV_LAUNCHER_OUTCOME_RESET;
    } else {
        s_launcher_state.execution_outcome = EV_LAUNCHER_OUTCOME_UNKNOWN;
    }
    s_launcher_state.selection_valid = false;
    s_main_entry_window = false;
    s_main_entry_consumed = false;
    if (s_launcher_state.trial_valid &&
            (s_launcher_state.trial_status == EV_LAUNCHER_TRIAL_ARMED ||
             s_launcher_state.trial_status == EV_LAUNCHER_TRIAL_RUNNING)) {
        s_launcher_state.trial_status = EV_LAUNCHER_TRIAL_FINISHED;
        s_launcher_state.trial_outcome = EV_LAUNCHER_OUTCOME_RESET;
    }
    portEXIT_CRITICAL(&s_launcher_mux);
}

void esp_vision_launcher_main_entry_begin(void)
{
    portENTER_CRITICAL(&s_launcher_mux);
    s_main_entry_window = true;
    s_main_entry_consumed = false;
    portEXIT_CRITICAL(&s_launcher_mux);
}

void esp_vision_launcher_main_entry_end(void)
{
    portENTER_CRITICAL(&s_launcher_mux);
    s_main_entry_window = false;
    s_main_entry_consumed = false;
    portEXIT_CRITICAL(&s_launcher_mux);
}

bool esp_vision_launcher_begin_active(uint64_t generation, const char *source_hash,
                                      const char *source_path)
{
    if (generation == 0 || generation > UINT64_C(9007199254740991) ||
            !ev_launcher_active_path(source_path, source_hash)) {
        return false;
    }
    // ev_session_vm_epoch takes a mutex: never call it in a port critical section.
    const uint32_t vm_epoch = ev_session_vm_epoch();
    portENTER_CRITICAL(&s_launcher_mux);
    const bool allowed = s_main_entry_window && !s_main_entry_consumed &&
                         !s_launcher_state.selection_valid &&
                         (s_launcher_state.execution_outcome == EV_LAUNCHER_OUTCOME_UNKNOWN ||
                          s_launcher_state.execution_outcome == EV_LAUNCHER_OUTCOME_RESET);
    if (allowed) {
        s_main_entry_consumed = true;
        memcpy(s_launcher_state.source_hash, source_hash, EV_LAUNCHER_SOURCE_HASH_HEX_LEN + 1);
        memcpy(s_launcher_state.source_path, source_path, strlen(source_path) + 1);
        s_launcher_state.generation = generation;
        s_launcher_state.vm_epoch = vm_epoch;
        s_launcher_state.selection_valid = true;
        s_launcher_state.execution_outcome = EV_LAUNCHER_OUTCOME_RUNNING;
    }
    portEXIT_CRITICAL(&s_launcher_mux);
    return allowed;
}

bool esp_vision_launcher_finish_active(ev_launcher_outcome_t outcome)
{
    if (outcome != EV_LAUNCHER_OUTCOME_NORMAL_RETURN &&
            outcome != EV_LAUNCHER_OUTCOME_UNCAUGHT_EXCEPTION &&
            outcome != EV_LAUNCHER_OUTCOME_USER_INTERRUPTION) {
        return false;
    }
    portENTER_CRITICAL(&s_launcher_mux);
    const bool valid = s_launcher_state.selection_valid &&
                       s_launcher_state.execution_outcome == EV_LAUNCHER_OUTCOME_RUNNING;
    if (valid) {
        s_launcher_state.execution_outcome = outcome;
    }
    portEXIT_CRITICAL(&s_launcher_mux);
    return valid;
}

bool esp_vision_launcher_arm_trial(const char *operation_id, const char *runner_path,
                                   const char *source_path, const char *source_hash)
{
    if (!ev_launcher_trial_paths(operation_id, runner_path, source_path, source_hash)) {
        return false;
    }
    const uint32_t vm_epoch = ev_session_vm_epoch();
    portENTER_CRITICAL(&s_launcher_mux);
    const bool allowed = (!s_launcher_state.trial_valid ||
                          s_launcher_state.trial_status == EV_LAUNCHER_TRIAL_FINISHED) &&
                         strcmp(s_launcher_state.last_trial_operation_id, operation_id) != 0;
    if (allowed) {
        memcpy(s_launcher_state.operation_id, operation_id, EV_LAUNCHER_OPERATION_ID_LEN + 1);
        memcpy(s_launcher_state.last_trial_operation_id, operation_id, EV_LAUNCHER_OPERATION_ID_LEN + 1);
        memcpy(s_launcher_state.trial_source_hash, source_hash, EV_LAUNCHER_SOURCE_HASH_HEX_LEN + 1);
        memcpy(s_launcher_state.trial_source_path, source_path, strlen(source_path) + 1);
        s_launcher_state.trial_vm_epoch = vm_epoch;
        s_launcher_state.trial_valid = true;
        s_launcher_state.trial_status = EV_LAUNCHER_TRIAL_ARMED;
        s_launcher_state.trial_outcome = EV_LAUNCHER_OUTCOME_UNKNOWN;
    }
    portEXIT_CRITICAL(&s_launcher_mux);
    return allowed;
}

bool esp_vision_launcher_begin_trial(const char *operation_id, const char *source_path,
                                     const char *source_hash)
{
    if (!ev_launcher_operation_id(operation_id) ||
            !ev_launcher_hex(source_hash, EV_LAUNCHER_SOURCE_HASH_HEX_LEN) ||
            source_path == NULL || strlen(source_path) > EV_LAUNCHER_SOURCE_PATH_MAX) {
        return false;
    }
    const uint32_t vm_epoch = ev_session_vm_epoch();
    portENTER_CRITICAL(&s_launcher_mux);
    const bool matches = s_launcher_state.trial_valid &&
                         s_launcher_state.trial_status == EV_LAUNCHER_TRIAL_ARMED &&
                         s_launcher_state.trial_vm_epoch == vm_epoch &&
                         strcmp(s_launcher_state.operation_id, operation_id) == 0 &&
                         strcmp(s_launcher_state.trial_source_path, source_path) == 0 &&
                         strcmp(s_launcher_state.trial_source_hash, source_hash) == 0;
    if (matches) {
        s_launcher_state.trial_status = EV_LAUNCHER_TRIAL_RUNNING;
        s_launcher_state.trial_outcome = EV_LAUNCHER_OUTCOME_RUNNING;
    }
    portEXIT_CRITICAL(&s_launcher_mux);
    return matches;
}

bool esp_vision_launcher_finish_trial(ev_launcher_outcome_t outcome)
{
    if (outcome != EV_LAUNCHER_OUTCOME_NORMAL_RETURN &&
            outcome != EV_LAUNCHER_OUTCOME_UNCAUGHT_EXCEPTION &&
            outcome != EV_LAUNCHER_OUTCOME_USER_INTERRUPTION) {
        return false;
    }
    portENTER_CRITICAL(&s_launcher_mux);
    const bool valid = s_launcher_state.trial_valid &&
                       s_launcher_state.trial_status == EV_LAUNCHER_TRIAL_RUNNING;
    if (valid) {
        s_launcher_state.trial_status = EV_LAUNCHER_TRIAL_FINISHED;
        s_launcher_state.trial_outcome = outcome;
    }
    portEXIT_CRITICAL(&s_launcher_mux);
    return valid;
}

bool esp_vision_launcher_cancel_trial_arm(const char *operation_id)
{
    if (!ev_launcher_operation_id(operation_id)) {
        return false;
    }
    portENTER_CRITICAL(&s_launcher_mux);
    const bool valid = s_launcher_state.trial_valid &&
                       s_launcher_state.trial_status == EV_LAUNCHER_TRIAL_ARMED &&
                       strcmp(s_launcher_state.operation_id, operation_id) == 0;
    if (valid) {
        s_launcher_state.trial_valid = false;
        s_launcher_state.trial_status = EV_LAUNCHER_TRIAL_NOT_STARTED;
        s_launcher_state.trial_outcome = EV_LAUNCHER_OUTCOME_UNKNOWN;
    }
    portEXIT_CRITICAL(&s_launcher_mux);
    return valid;
}

bool esp_vision_launcher_state_snapshot(ev_launcher_state_snapshot_t *out)
{
    if (out == NULL || !s_initialized) {
        return false;
    }
    portENTER_CRITICAL(&s_launcher_mux);
    memcpy(out, &s_launcher_state, sizeof(*out));
    portEXIT_CRITICAL(&s_launcher_mux);
    out->vm_epoch = ev_session_vm_epoch();
    return true;
}

bool esp_vision_launcher_trial_snapshot(const char *operation_id,
                                        ev_launcher_state_snapshot_t *out)
{
    if (out == NULL || !ev_launcher_operation_id(operation_id) ||
            !esp_vision_launcher_state_snapshot(out)) {
        return false;
    }
    return out->trial_valid && strcmp(out->operation_id, operation_id) == 0;
}

const char *esp_vision_launcher_outcome_name(ev_launcher_outcome_t outcome)
{
    switch (outcome) {
    case EV_LAUNCHER_OUTCOME_RUNNING: return "running";
    case EV_LAUNCHER_OUTCOME_NORMAL_RETURN: return "normal_return";
    case EV_LAUNCHER_OUTCOME_UNCAUGHT_EXCEPTION: return "uncaught_exception";
    case EV_LAUNCHER_OUTCOME_USER_INTERRUPTION: return "user_interruption";
    case EV_LAUNCHER_OUTCOME_RESET: return "reset";
    default: return "unknown";
    }
}

const char *esp_vision_launcher_trial_status_name(ev_launcher_trial_status_t status)
{
    switch (status) {
    case EV_LAUNCHER_TRIAL_ARMED: return "armed";
    case EV_LAUNCHER_TRIAL_RUNNING: return "running";
    case EV_LAUNCHER_TRIAL_FINISHED: return "finished";
    default: return "not_started";
    }
}

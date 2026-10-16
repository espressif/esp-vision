/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ev_file_transfer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "storage.h"

#define FILE_PATH_CAP (256)
#define FILE_IDLE_MS (30000U)

typedef struct {
    bool active;
    bool poisoned;
    bool overwrite;
    int fd;
    esp_vision_storage_volume_t volume;
    ev_session_snapshot_t owner;
    uint8_t id[16];
    uint8_t expected_hash[32];
    uint32_t total;
    uint32_t received;
    uint32_t last_ms;
    char target[FILE_PATH_CAP];
    char temporary[FILE_PATH_CAP];
    psa_hash_operation_t hash;
} file_transfer_t;

static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static file_transfer_t s_file = { .fd = -1 };

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool same_owner(const ev_session_snapshot_t *a, const ev_session_snapshot_t *b)
{
    return a->has_owner && b->has_owner && b->vm_ready && b->handshake_complete &&
           a->owner_link.transport_id == b->owner_link.transport_id &&
           a->owner_link.slot == b->owner_link.slot &&
           a->owner_link.generation == b->owner_link.generation &&
           a->vm_epoch == b->vm_epoch && a->input_generation == b->input_generation &&
           a->transition_id == b->transition_id && strcmp(a->session_id, b->session_id) == 0;
}

static bool current_owner(const ev_session_snapshot_t *owner)
{
    ev_session_snapshot_t current;
    return ev_session_snapshot(&current) && same_owner(owner, &current);
}

static bool unhex(const char *text, uint8_t *bytes, size_t length)
{
    if (text == NULL || strlen(text) != length * 2) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned int value = 0;
        for (size_t j = 0; j < 2; ++j) {
            char c = text[i * 2 + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
            value = value * 16 + (unsigned int)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        bytes[i] = (uint8_t)value;
    }
    return true;
}

/* Hold one admitted VFS operation for the whole native descriptor lifetime.
 * MSC transitions cannot hand this volume away while that drain count is live.
 * A failed close retains the barrier instead of claiming a clean handoff. */
static const char *discard(void)
{
    if (s_file.poisoned) {
        return "FILE_CLOSE_FAILED";
    }
    if (s_file.fd >= 0) {
        int result = close(s_file.fd);
        s_file.fd = -1;
        if (result != 0) {
            s_file.poisoned = true;
            return "FILE_CLOSE_FAILED";
        }
    }
    if (s_file.temporary[0] != '\0') {
        (void)unlink(s_file.temporary);
    }
    (void)psa_hash_abort(&s_file.hash);
    bool finished = !s_file.active || esp_vision_msc_writer_vfs_finish(s_file.volume);
    memset(&s_file, 0, sizeof(s_file));
    s_file.fd = -1;
    return finished ? NULL : "STORAGE_QUARANTINED";
}

bool ev_file_transfer_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    }
    return s_lock != NULL && psa_crypto_init() == PSA_SUCCESS;
}

const char *ev_file_transfer_begin(const ev_session_snapshot_t *owner, const char *path,
                                   bool overwrite, uint32_t total, const char *sha256,
                                   const char upload_id[33])
{
    if (s_lock == NULL) {
        return "TRANSFER_UNAVAILABLE";
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *error = NULL;
    uint8_t digest[32], token[16];
    bool path_safe = path != NULL;
    if (path != NULL) {
        for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; ++p) {
            if (*p < 0x20 || *p == 0x7f || *p == '\\' || *p == '"') {
                path_safe = false;
            }
        }
    }
    if (!current_owner(owner)) {
        error = "SESSION_LOST";
    } else if (s_file.active) {
        error = "TRANSFER_BUSY";
    } else if (!path_safe || path[0] != '/' || strlen(path) > 123 || path[1] == '\0' ||
               path[strlen(path) - 1] == '/' || strstr(path, "..") != NULL || strstr(path, "//") != NULL || strcmp(path, ESP_VISION_STORAGE_SDCARD_PATH) == 0 ||
               !unhex(sha256, digest, sizeof(digest)) || !unhex(upload_id, token, sizeof(token))) {
        error = "INVALID_ARGUMENT";
    } else {
        memset(&s_file, 0, sizeof(s_file));
        s_file.fd = -1;
        s_file.hash = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
        s_file.volume = ESP_VISION_STORAGE_VOLUME_FLASH;
        const size_t sd_len = strlen(ESP_VISION_STORAGE_SDCARD_PATH);
        if (strncmp(path, ESP_VISION_STORAGE_SDCARD_PATH, sd_len) == 0 && path[sd_len] == '/') {
            s_file.volume = ESP_VISION_STORAGE_VOLUME_SD;
        }
        int count = snprintf(s_file.target, sizeof(s_file.target), "%s%s",
                             s_file.volume == ESP_VISION_STORAGE_VOLUME_FLASH ? ESP_VISION_STORAGE_FLASH_PATH : "", path);
        if (count < 0 || (size_t)count >= sizeof(s_file.target)) {
            error = "INVALID_PATH";
        } else if (!esp_vision_msc_writer_vfs_begin(s_file.volume, true)) {
            error = "STORAGE_READ_ONLY";
        } else {
            s_file.active = true;
            s_file.owner = *owner;
            s_file.overwrite = overwrite;
            s_file.total = total;
            s_file.last_ms = now_ms();
            memcpy(s_file.expected_hash, digest, sizeof(digest));
            struct stat existing;
            int status = stat(s_file.target, &existing);
            if (status == 0 && (!overwrite || !S_ISREG(existing.st_mode))) {
                error = "FILE_EXISTS";
            } else if (status != 0 && errno != ENOENT) {
                error = "FILE_STAT_FAILED";
            } else {
                memcpy(s_file.id, token, sizeof(token));
                count = snprintf(s_file.temporary, sizeof(s_file.temporary), "%s.evupload-%s.tmp", s_file.target, upload_id);
                if (count < 0 || (size_t)count >= sizeof(s_file.temporary)) {
                    error = "INVALID_PATH";
                } else {
                    s_file.fd = open(s_file.temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
                    if (s_file.fd < 0) {
                        /* Never unlink a pre-existing staging file. */
                        s_file.temporary[0] = '\0';
                        error = "FILE_OPEN_FAILED";
                    } else if (psa_hash_setup(&s_file.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
                        error = "HASH_FAILED";
                    }
                }
            }
            if (error != NULL) {
                (void)discard();
            }
        }
    }
    xSemaphoreGive(s_lock);
    return error;
}

const char *ev_file_transfer_write(const ev_session_snapshot_t *owner, const uint8_t *payload,
                                   size_t length, uint32_t *received)
{
    if (s_lock == NULL) {
        return "TRANSFER_UNAVAILABLE";
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *error = NULL;
    uint32_t offset = 0;
    if (length >= EV_FILE_PREFIX_BYTES) {
        for (size_t i = 0; i < 4; ++i) {
            offset |= (uint32_t)payload[16 + i] << (8 * i);
        }
    }
    if (!s_file.active || !same_owner(&s_file.owner, owner) || !current_owner(owner) ||
            length <= EV_FILE_PREFIX_BYTES || length > EV_FILE_PAYLOAD_MAX ||
            memcmp(payload, s_file.id, sizeof(s_file.id)) != 0) {
        error = "INVALID_TRANSFER";
    } else if (s_file.poisoned) {
        error = "FILE_CLOSE_FAILED";
    } else {
        const size_t size = length - EV_FILE_PREFIX_BYTES;
        if (offset != s_file.received || size > s_file.total - s_file.received ||
                (size % 4096 != 0 && size != s_file.total - s_file.received)) {
            error = "INVALID_OFFSET_OR_SIZE";
        } else {
            size_t written = 0;
            while (written < size) {
                ssize_t count = write(s_file.fd, payload + EV_FILE_PREFIX_BYTES + written, size - written);
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count <= 0) {
                    error = "FILE_WRITE_FAILED";
                    break;
                }
                written += (size_t)count;
            }
            if (error == NULL && psa_hash_update(&s_file.hash, payload + EV_FILE_PREFIX_BYTES, size) != PSA_SUCCESS) {
                error = "HASH_FAILED";
            }
            if (error == NULL && !current_owner(owner)) {
                error = "SESSION_LOST";
            }
            if (error == NULL) {
                s_file.received += (uint32_t)size;
                s_file.last_ms = now_ms();
                *received = s_file.received;
            } else {
                (void)discard();
            }
        }
    }
    xSemaphoreGive(s_lock);
    return error;
}

/* FatFS cannot replace an existing destination in one rename. Keep the old
 * regular file as a rollback backup until publication succeeds. This protects
 * ordinary commit failures; it is not a power-loss journal or atomic replace. */
static const char *publish(void)
{
    struct stat existing;
    bool exists = stat(s_file.target, &existing) == 0;
    if (!exists && errno != ENOENT) {
        return "FILE_STAT_FAILED";
    }
    if (exists && (!s_file.overwrite || !S_ISREG(existing.st_mode))) {
        return "FILE_EXISTS";
    }
    if (rename(s_file.temporary, s_file.target) == 0) {
        s_file.temporary[0] = '\0';
        return NULL;
    }
    if (!exists || errno != EEXIST) {
        return "FILE_RENAME_FAILED";
    }
    char backup[FILE_PATH_CAP];
    int count = snprintf(backup, sizeof(backup), "%s.old", s_file.temporary);
    if (count < 0 || (size_t)count >= sizeof(backup) || stat(backup, &existing) == 0 || errno != ENOENT ||
            rename(s_file.target, backup) != 0) {
        return "FILE_RENAME_FAILED";
    }
    if (rename(s_file.temporary, s_file.target) != 0) {
        return rename(backup, s_file.target) == 0 ? "FILE_RENAME_FAILED" : "FILE_ROLLBACK_FAILED";
    }
    s_file.temporary[0] = '\0';
    /* Failure here retains a complete old backup, never deletes the new file. */
    (void)unlink(backup);
    return NULL;
}

const char *ev_file_transfer_end(const ev_session_snapshot_t *owner, const char *upload_id,
                                 bool commit, uint32_t *received)
{
    if (s_lock == NULL) {
        return "TRANSFER_UNAVAILABLE";
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t id[16];
    const char *error = NULL;
    if (!s_file.active || !same_owner(&s_file.owner, owner) || !current_owner(owner) ||
            !unhex(upload_id, id, sizeof(id)) || memcmp(id, s_file.id, sizeof(id)) != 0) {
        error = "INVALID_TRANSFER";
    } else if (s_file.poisoned) {
        error = "FILE_CLOSE_FAILED";
    } else if (!commit) {
        error = discard();
        *received = 0;
    } else if (s_file.received != s_file.total) {
        error = "FILE_INCOMPLETE";
    } else {
        uint8_t hash[32];
        size_t hash_length = 0;
        if (psa_hash_finish(&s_file.hash, hash, sizeof(hash), &hash_length) != PSA_SUCCESS ||
                hash_length != sizeof(hash) || memcmp(hash, s_file.expected_hash, sizeof(hash)) != 0) {
            error = "HASH_MISMATCH";
        } else if (fsync(s_file.fd) != 0) {
            error = "FILE_SYNC_FAILED";
        } else if (!current_owner(owner)) {
            error = "SESSION_LOST";
        } else {
            int result = close(s_file.fd);
            s_file.fd = -1;
            if (result != 0) {
                s_file.poisoned = true;
                error = "FILE_CLOSE_FAILED";
            } else {
                error = publish();
                *received = s_file.received;
            }
        }
        const char *cleanup_error = discard();
        if (error == NULL) {
            error = cleanup_error;
        }
    }
    xSemaphoreGive(s_lock);
    return error;
}

void ev_file_transfer_poll(void)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    if (s_file.active && (!current_owner(&s_file.owner) ||
                          (uint32_t)(now_ms() - s_file.last_ms) >= FILE_IDLE_MS)) {
        (void)discard();
    }
    xSemaphoreGive(s_lock);
}

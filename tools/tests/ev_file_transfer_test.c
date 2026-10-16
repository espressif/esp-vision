/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/evp.h>
#include "ev_file_transfer.h"
#include "storage_writer.h"

static ev_session_snapshot_t current;
static uint32_t clock_ms;
static unsigned int gate, closes, writes;
static bool deny_gate, fail_write, fail_sync, fail_close, fail_publish;
static size_t partial_write;
static const char *root;

bool ev_session_snapshot(ev_session_snapshot_t *out)
{
    *out = current;
    return true;
}
int64_t esp_timer_get_time(void)
{
    return (int64_t)clock_ms * 1000;
}

bool esp_vision_msc_writer_vfs_begin(esp_vision_storage_volume_t volume, bool writing)
{
    assert(writing);
    if (deny_gate || volume != ESP_VISION_STORAGE_VOLUME_FLASH) {
        return false;
    } ++gate;
    return true;
}
bool esp_vision_msc_writer_vfs_finish(esp_vision_storage_volume_t volume)
{
    assert(volume == ESP_VISION_STORAGE_VOLUME_FLASH && gate == 1);
    --gate;
    return true;
}
ssize_t ev_test_write(int fd, const void *buffer, size_t size)
{
    assert(gate == 1);
    ++writes;
    if (fail_write) {
        errno = ENOSPC;
        return -1;
    } if (partial_write && size > partial_write) {
        size = partial_write;
    } return write(fd, buffer, size);
}
int ev_test_close(int fd)
{
    ++closes;
    if (fail_close) {
        errno = EIO;
        return -1;
    } return close(fd);
}
int ev_test_fsync(int fd)
{
    if (fail_sync) {
        errno = EIO;
        return -1;
    } return fsync(fd);
}
int ev_test_rename(const char *src, const char *dst)
{
    struct stat st;
    if (stat(dst, &st) == 0) {
        errno = EEXIST;    /* FatFS, unlike POSIX. */
        return -1;
    }
    if (fail_publish && strstr(src, ".tmp") && !strstr(src, ".old") && !strstr(dst, ".old")) {
        errno = EIO;
        return -1;
    }
    return rename(src, dst);
}

static void digest(const uint8_t *data, size_t size, char output[65])
{
    unsigned char hash[32]; unsigned int length;
    EVP_MD_CTX *context = EVP_MD_CTX_new(); assert(context);
    assert(EVP_DigestInit_ex(context, EVP_sha256(), NULL));
    assert(EVP_DigestUpdate(context, data, size)); assert(EVP_DigestFinal_ex(context, hash, &length));
    EVP_MD_CTX_free(context); assert(length == 32);
    for (size_t i = 0; i < 32; ++i) {
        snprintf(output + i * 2, 3, "%02x", hash[i]);
    }
}
static void physical(char *output, const char *path)
{
    assert(snprintf(output, 512, "%s%s", root, path) < 512);
}
static void seed_old(void)
{
    char path[512];
    physical(path, "/target");
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite("OLD", 1, 3, file) == 3);
    assert(fclose(file) == 0);
}
static void assert_old(void)
{
    char path[512], bytes[8] = {0};
    physical(path, "/target");
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(bytes, 1, 8, file) == 3);
    assert(strcmp(bytes, "OLD") == 0);
    fclose(file);
}
static void packet(uint8_t *out, const char *id, uint32_t offset, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < 16; ++i) {
        unsigned int value;
        assert(sscanf(id + i * 2, "%2x", &value) == 1);
        out[i] = (uint8_t)value;
    }
    for (size_t i = 0; i < 4; ++i) {
        out[16 + i] = (uint8_t)(offset >> (8 * i));
    }
    memcpy(out + 20, data, size);
}
static void begin(char id[33], const uint8_t *data, size_t size)
{
    static unsigned int sequence;
    ++sequence;
    for (size_t i = 0; i < 16; ++i) {
        snprintf(id + i * 2, 3, "%02x", (sequence + (unsigned int)i) & 255);
    }
    char hash[65]; digest(data, size, hash);
    assert(ev_file_transfer_begin(&current, "/target", true, (uint32_t)size, hash, id) == NULL);
    assert(gate == 1);
}
static void assert_error(const char *actual, const char *expected)
{
    assert(actual);
    if (strcmp(actual, expected)) {
        fprintf(stderr, "expected %s got %s\n", expected, actual);
        abort();
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2); root = argv[1];
    current.has_owner = current.vm_ready = current.handshake_complete = true;
    current.owner_link.transport_id = EV_TRANSPORT_USJ; current.owner_link.generation = 1;
    current.vm_epoch = current.transition_id = current.input_generation = 1;
    strcpy(current.session_id, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    assert(ev_file_transfer_init());
    uint8_t *data = malloc(2 * 1024 * 1024); assert(data);
    for (size_t i = 0; i < 2 * 1024 * 1024; ++i) {
        data[i] = (uint8_t)(i * 73 + i / 256 * 17);
    }
    uint8_t frame[EV_FILE_PAYLOAD_MAX]; char id[33], other[33], hash[65]; uint32_t received;
    seed_old();
    begin(id, data, 2 * 1024 * 1024);
    for (uint32_t offset = 0; offset < 2 * 1024 * 1024; offset += EV_FILE_CHUNK_BYTES) {
        packet(frame, id, offset, data + offset, EV_FILE_CHUNK_BYTES);
        assert(ev_file_transfer_write(&current, frame, sizeof(frame), &received) == NULL);
        assert(received == offset + EV_FILE_CHUNK_BYTES); assert(gate == 1 && closes == 0);
    }
    assert(ev_file_transfer_end(&current, id, true, &received) == NULL);
    assert(received == 2 * 1024 * 1024 && gate == 0 && closes == 1 && writes == 128);
    char path[512]; physical(path, "/target"); FILE *file = fopen(path, "rb"); assert(file);
    for (size_t i = 0; i < 2 * 1024 * 1024; ++i) {
        assert(fgetc(file) == data[i]);
    }
    assert(fgetc(file) == EOF); fclose(file);
    seed_old(); digest(data, 4096, hash);
    assert_error(ev_file_transfer_begin(&current, "/target", false, 4096, hash, id), "FILE_EXISTS"); assert(gate == 0); assert_old();
    begin(id, data, 4096);
    assert_error(ev_file_transfer_begin(&current, "/other", true, 4096, hash, other), "TRANSFER_BUSY");
    packet(frame, id, 1, data, 4096); assert_error(ev_file_transfer_write(&current, frame, 4116, &received), "INVALID_OFFSET_OR_SIZE");
    packet(frame, id, 0, data, 12); assert_error(ev_file_transfer_write(&current, frame, 32, &received), "INVALID_OFFSET_OR_SIZE");
    assert_error(ev_file_transfer_end(&current, id, true, &received), "FILE_INCOMPLETE");
    packet(frame, id, 0, data, 4096); assert(ev_file_transfer_write(&current, frame, 4116, &received) == NULL);
    assert_error(ev_file_transfer_write(&current, frame, 4116, &received), "INVALID_OFFSET_OR_SIZE");
    assert(ev_file_transfer_end(&current, id, false, &received) == NULL); assert(gate == 0); assert_old();
    memset(hash, '0', 64); hash[64] = 0;
    assert(ev_file_transfer_begin(&current, "/target", true, 4096, hash, id) == NULL);
    packet(frame, id, 0, data, 4096); assert(ev_file_transfer_write(&current, frame, 4116, &received) == NULL);
    assert_error(ev_file_transfer_end(&current, id, true, &received), "HASH_MISMATCH"); assert(gate == 0); assert_old();
    begin(id, data, 4096); fail_write = true;
    packet(frame, id, 0, data, 4096); assert_error(ev_file_transfer_write(&current, frame, 4116, &received), "FILE_WRITE_FAILED"); fail_write = false; assert(gate == 0); assert_old();
    begin(id, data, 4096); partial_write = 123;
    packet(frame, id, 0, data, 4096); assert(ev_file_transfer_write(&current, frame, 4116, &received) == NULL); partial_write = 0;
    fail_sync = true; assert_error(ev_file_transfer_end(&current, id, true, &received), "FILE_SYNC_FAILED"); fail_sync = false; assert(gate == 0); assert_old();
    begin(id, data, 4096); packet(frame, id, 0, data, 4096); assert(ev_file_transfer_write(&current, frame, 4116, &received) == NULL);
    fail_publish = true; assert_error(ev_file_transfer_end(&current, id, true, &received), "FILE_RENAME_FAILED"); fail_publish = false; assert(gate == 0); assert_old();
    begin(id, data, 4096); ++current.vm_epoch; ev_file_transfer_poll(); assert(gate == 0); assert_old();
    begin(id, data, 4096); clock_ms += 30001; ev_file_transfer_poll(); assert(gate == 0); assert_old();
    begin(id, data, 0); assert(ev_file_transfer_end(&current, id, true, &received) == NULL); assert(received == 0 && gate == 0);
    deny_gate = true; digest(data, 4096, hash); assert_error(ev_file_transfer_begin(&current, "/target", true, 4096, hash, id), "STORAGE_READ_ONLY"); deny_gate = false;
    assert_error(ev_file_transfer_begin(&current, "/../bad", true, 4096, hash, id), "INVALID_ARGUMENT");
    begin(id, data, 4096); fail_close = true; assert_error(ev_file_transfer_end(&current, id, false, &received), "FILE_CLOSE_FAILED"); assert(gate == 1);
    assert_error(ev_file_transfer_begin(&current, "/other", true, 4096, hash, other), "TRANSFER_BUSY");
    free(data); puts("binary file engine: integrity, offsets, partial IO, FAT replacement/rollback, lifecycle and close barrier passed");
}

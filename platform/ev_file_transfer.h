/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ev_session.h"

#define EV_FILE_CHUNK_BYTES (16384U)
#define EV_FILE_PREFIX_BYTES (20U) /* 16-byte upload ID, little-endian uint32 offset */
#define EV_FILE_PAYLOAD_MAX (EV_FILE_CHUNK_BYTES + EV_FILE_PREFIX_BYTES)

bool ev_file_transfer_init(void);
const char *ev_file_transfer_begin(const ev_session_snapshot_t *owner, const char *path,
                                   bool overwrite, uint32_t total, const char *sha256,
                                   const char upload_id[33]);
const char *ev_file_transfer_write(const ev_session_snapshot_t *owner, const uint8_t *payload,
                                   size_t length, uint32_t *received);
const char *ev_file_transfer_end(const ev_session_snapshot_t *owner, const char *upload_id,
                                 bool commit, uint32_t *received);
void ev_file_transfer_poll(void);

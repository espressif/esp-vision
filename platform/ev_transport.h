/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Physical EV-MUX endpoint types. Local console/null outputs are not links.
typedef enum {
    EV_TRANSPORT_USJ = 0,
    EV_TRANSPORT_USB,
    EV_TRANSPORT_UART,
    EV_TRANSPORT_TCP,
    EV_TRANSPORT_MAX,
} ev_transport_id_t;

#define EV_TRANSPORT_PHYSICAL_SLOT (0)
#define EV_TRANSPORT_TCP_LINKS (2)
#define EV_TRANSPORT_LINK_SLOTS_MAX (EV_TRANSPORT_TCP_LINKS)

// A link reference is an endpoint identity, not an fd. A ref is valid only
// while its slot is open and its generation still matches the live slot.
typedef struct {
    ev_transport_id_t transport_id;
    uint8_t slot;
    uint32_t generation;
} ev_link_ref_t;

typedef enum {
    EV_IO_PROGRESS = 0,
    EV_IO_WOULD_BLOCK,
    EV_IO_CLOSED,
    EV_IO_ERROR,
} ev_io_status_t;

typedef struct {
    ev_io_status_t status;
    size_t transferred;
    esp_err_t error;
} ev_io_result_t;

typedef struct {
    bool supported;
    bool present_known;
    bool present;
    bool connected_known;
    bool connected;
    bool ready;
    bool owner;
} ev_transport_endpoint_state_t;

typedef ev_io_result_t (*ev_transport_read_cb_t)(const ev_link_ref_t *link,
                                                 void *buffer,
                                                 size_t capacity);
typedef ev_io_result_t (*ev_transport_write_cb_t)(const ev_link_ref_t *link,
                                                  const void *buffer,
                                                  size_t length);
typedef void (*ev_transport_link_close_cb_t)(const ev_link_ref_t *link);

// Per-link I/O is nonblocking. A write may make short progress or return
// WOULD_BLOCK; callers retain the unsent suffix and yield before retrying.
// These callback types are shared by the statically compiled endpoint
// backends; this is not a runtime plugin registration interface.
typedef struct {
    ev_transport_read_cb_t read;
    ev_transport_write_cb_t write;
    ev_transport_link_close_cb_t close;
} ev_transport_link_ops_t;

esp_err_t ev_transport_init0(void);
const char *ev_transport_name(ev_transport_id_t id);
bool ev_transport_id_from_name(const char *name, ev_transport_id_t *out_id);

// Return the currently live reference for an endpoint slot. Serial endpoints
// use slot 0; TCP has up to EV_TRANSPORT_TCP_LINKS independently generated
// slots. TCP accept owns opening a slot; disconnect closes it.
bool ev_transport_link_ref(ev_transport_id_t id, uint8_t slot, ev_link_ref_t *out_link);
bool ev_transport_link_is_current(const ev_link_ref_t *link);
esp_err_t ev_transport_link_open(ev_transport_id_t id, uint8_t slot, ev_link_ref_t *out_link);
void ev_transport_link_close(const ev_link_ref_t *link);
bool ev_transport_link_advance_generation(const ev_link_ref_t *link, ev_link_ref_t *out_link);

// Read and write do one nonblocking backend operation. Invalid/stale refs
// return CLOSED. For a successful PROGRESS result, transferred is nonzero and
// no larger than the requested capacity/length.
ev_io_result_t ev_transport_read(const ev_link_ref_t *link, void *buffer, size_t capacity);
ev_io_result_t ev_transport_write(const ev_link_ref_t *link, const void *buffer, size_t length);

void ev_transport_endpoint_state(ev_transport_id_t id, uint8_t slot,
                                 ev_transport_endpoint_state_t *out_state);
void ev_transport_set_owner(const ev_link_ref_t *owner_or_null);

#ifdef __cplusplus
}
#endif

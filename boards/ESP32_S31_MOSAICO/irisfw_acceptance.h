/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "ev_transport.h"

/* Transport-task-only installer compatibility. Feed only bytes outside an
 * EV-MUX frame; an EV-MUX SOF discards a partial installer line.
 */
void esp_vision_irisfw_acceptance_byte(const ev_link_ref_t *link, uint8_t byte, uint32_t now_ms);
void esp_vision_irisfw_acceptance_frame_start(void);
void esp_vision_irisfw_acceptance_poll(uint32_t now_ms);

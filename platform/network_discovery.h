/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Call before starting workers; all later mDNS changes share one mutex. */
void esp_vision_discovery_init(void);
esp_err_t esp_vision_discovery_ensure(void);
/* TCP worker calls this after listener synchronization; retries are idempotent. */
void esp_vision_discovery_set_listening(bool listening);

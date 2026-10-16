/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <sys/types.h>

typedef size_t mp_uint_t;

ssize_t mp_hal_stdout_tx_strn(const char *str, mp_uint_t len);

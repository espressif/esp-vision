// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include_next "ZXConfig.h"

#undef ZX_THREAD_LOCAL
#define ZX_THREAD_LOCAL static

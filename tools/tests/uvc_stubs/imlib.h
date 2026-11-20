/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#define PIXFORMAT_JPEG (1)

struct image {
    int w;
    int h;
    int pixfmt;
};

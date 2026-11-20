/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

// Clear Python-owned UVC callback state before the VM heap is collected during
// soft reset. This symbol is present even on builds without USB-device UVC.
void py_uvc_soft_reset(void);

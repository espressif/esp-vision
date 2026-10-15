/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESP_VISION_TEST_PORTMACRO_H
#define ESP_VISION_TEST_PORTMACRO_H

#include <assert.h>

typedef int portMUX_TYPE;
extern int ev_test_in_critical;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) do { (void)(mux); assert(!ev_test_in_critical); ev_test_in_critical = 1; } while (0)
#define portEXIT_CRITICAL(mux) do { (void)(mux); assert(ev_test_in_critical); ev_test_in_critical = 0; } while (0)

#endif

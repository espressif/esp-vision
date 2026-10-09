# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

# ESP32_P4X_FUNCTION_EV_BOARD (GT911) or ESP32_S31_MOSAICO (CST9220).
# The display does not need to be initialized to read touch coordinates.
import time
import lcd_touch

tp = lcd_touch.Touch()
try:
    while True:
        points = tp.read()
        if points:
            print(points)
        time.sleep_ms(20)
finally:
    tp.deinit()

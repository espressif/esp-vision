# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import display
import sensor
import time


lcd = display.Display()

try:
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QVGA)
    sensor.skip_frames(time=1000)

    while True:
        img = sensor.snapshot()
        lcd.write(img)
        time.sleep_ms(20)
finally:
    sensor.shutdown()
    lcd.deinit()

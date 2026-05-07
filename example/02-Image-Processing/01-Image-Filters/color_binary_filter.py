# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


RED_THRESHOLD = [(30, 100, 15, 127, 15, 127)]

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

while True:
    img = sensor.snapshot()
    img.binary(RED_THRESHOLD)
    img.draw_string(2, 2, "color binary", color=(255, 255, 255))
    img.flush()
    time.sleep_ms(20)

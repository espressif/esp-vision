# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


THRESHOLDS = [(80, 255)]

sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

frame = 0

while True:
    img = sensor.snapshot()
    img.binary(THRESHOLDS)
    if frame % 2:
        img.invert()
        img.draw_string(2, 2, "invert", color=255)
    else:
        img.draw_string(2, 2, "binary", color=255)
    img.flush()
    frame += 1
    time.sleep_ms(20)

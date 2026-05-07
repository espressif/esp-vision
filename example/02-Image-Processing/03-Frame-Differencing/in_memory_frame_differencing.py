# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


THRESHOLDS = [(35, 255)]

sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

background = sensor.snapshot().copy()

while True:
    img = sensor.snapshot()
    img.difference(background)
    img.binary(THRESHOLDS)
    img.draw_string(2, 2, "frame diff", color=255)
    img.flush()
    time.sleep_ms(20)

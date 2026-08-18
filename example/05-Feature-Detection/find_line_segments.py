# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.QQVGA)
sensor.skip_frames(time=1000)

while True:
    img = sensor.snapshot()

    for line in img.find_line_segments(threshold=50):
        img.draw_line(line.line(), color=255, thickness=2)

    img.flush()
    time.sleep_ms(20)

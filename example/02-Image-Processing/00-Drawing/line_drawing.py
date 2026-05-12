# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

frame = 0

while True:
    img = sensor.snapshot()

    sweep = frame % 180
    y0 = 20 + (frame % 120)
    y1 = 40 + ((frame * 2) % 160)

    img.draw_line(10, y0, 300, 20, color=(255, 0, 0), thickness=2)
    img.draw_line(10, 60, 300 - sweep, y1, color=(0, 255, 0), thickness=2)
    img.draw_arrow(20, 210, 150 + (frame % 140), 170 - (frame % 80), color=(255, 255, 0), thickness=2)
    img.draw_string(10, 90, "line drawing %d" % frame, color=(255, 255, 255))

    img.flush()
    frame = (frame + 1) % 1000
    time.sleep_ms(20)

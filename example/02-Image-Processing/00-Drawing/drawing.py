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

    x = (frame * 4) % 220
    y = 20 + ((frame * 3) % 120)
    radius = 12 + (frame % 24)

    img.draw_line(10, 10 + (frame % 80), 120 + (frame % 120), 70, color=(255, 0, 0), thickness=2)
    img.draw_rectangle(x, 20, 80, 60, color=(0, 255, 0), thickness=2)
    img.draw_circle(80 + (frame % 120), 160, radius, color=(0, 0, 255), thickness=2)
    img.draw_cross(240, y, color=(255, 255, 0), size=12, thickness=2)
    img.draw_arrow(20, 220, 140 + (frame % 120), 200 - (frame % 80), color=(255, 0, 255), thickness=2)
    img.draw_string(10, 90, "imlib draw %d" % frame, color=(255, 255, 255))

    img.flush()
    frame = (frame + 1) % 1000
    time.sleep_ms(20)

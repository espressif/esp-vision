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

    x = 20 + ((frame * 3) % 180)
    y = 30 + ((frame * 2) % 120)
    radius = 16 + (frame % 24)
    angle = (frame * 5) % 180

    img.draw_rectangle(x, 20, 90, 60, color=(255, 0, 0), thickness=2)
    img.draw_circle(180, y, radius, color=(0, 255, 0), thickness=2)
    img.draw_ellipse(245, 145, 45, 25, angle, color=(0, 0, 255), thickness=2)
    img.draw_cross(70 + (frame % 160), 170, color=(255, 255, 0), size=16, thickness=2)
    img.draw_string(10, 220, "shape drawing %d" % frame, color=(255, 255, 255))

    img.flush()
    frame = (frame + 1) % 1000
    time.sleep_ms(20)

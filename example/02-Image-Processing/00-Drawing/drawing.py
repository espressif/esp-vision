# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

while True:
    img = sensor.snapshot()
    img.draw_line(10, 10, 120, 70, color=(255, 0, 0), thickness=2)
    img.draw_rectangle(140, 20, 80, 60, color=(0, 255, 0), thickness=2)
    img.draw_circle(80, 160, 35, color=(0, 0, 255), thickness=2)
    img.draw_cross(240, 160, color=(255, 255, 0), size=12, thickness=2)
    img.draw_arrow(20, 220, 140, 200, color=(255, 0, 255), thickness=2)
    img.draw_string(10, 90, "imlib draw", color=(255, 255, 255))
    img.flush()
    time.sleep_ms(20)

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
    img.draw_rectangle(20, 20, 90, 60, color=(255, 0, 0), thickness=2)
    img.draw_circle(180, 55, 35, color=(0, 255, 0), thickness=2)
    img.draw_ellipse(245, 145, 45, 25, 20, color=(0, 0, 255), thickness=2)
    img.draw_cross(70, 170, color=(255, 255, 0), size=16, thickness=2)
    img.draw_string(10, 220, "shape drawing", color=(255, 255, 255))
    img.flush()
    time.sleep_ms(20)

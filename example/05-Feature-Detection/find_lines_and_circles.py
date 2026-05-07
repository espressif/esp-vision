# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

while True:
    img = sensor.snapshot()

    for line in img.find_lines(threshold=1400, theta_margin=25, rho_margin=25):
        img.draw_line(line.line(), color=255, thickness=2)

    for circle in img.find_circles(threshold=2500, r_min=8, r_max=80, r_step=4):
        img.draw_circle(circle.x(), circle.y(), circle.r(), color=255, thickness=2)

    img.flush()
    time.sleep_ms(20)

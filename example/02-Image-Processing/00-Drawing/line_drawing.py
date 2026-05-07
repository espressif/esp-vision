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
    img.draw_line(10, 20, 300, 20, color=(255, 0, 0), thickness=2)
    img.draw_line(10, 60, 300, 120, color=(0, 255, 0), thickness=2)
    img.draw_arrow(20, 210, 150, 170, color=(255, 255, 0), thickness=2)
    img.draw_string(10, 90, "line drawing", color=(255, 255, 255))
    img.flush()
    time.sleep_ms(20)

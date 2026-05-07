# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


THRESHOLDS = [(80, 255)]
OPS = ("erode", "dilate", "open", "close")

sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

frame = 0

while True:
    name = OPS[(frame // 30) % len(OPS)]
    img = sensor.snapshot()
    img.binary(THRESHOLDS)

    if name == "erode":
        img.erode(1)
    elif name == "dilate":
        img.dilate(1)
    elif name == "open":
        img.open(1)
    elif name == "close":
        img.close(1)

    img.draw_string(2, 2, name, color=255)
    img.flush()
    frame += 1
    time.sleep_ms(20)

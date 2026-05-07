# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


OPS = ("mean", "median", "mode", "midpoint", "gaussian", "laplacian", "bilateral", "histeq")

sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

frame = 0

while True:
    name = OPS[(frame // 30) % len(OPS)]
    img = sensor.snapshot()

    if name == "mean":
        img.mean(1)
    elif name == "median":
        img.median(1)
    elif name == "mode":
        img.mode(1)
    elif name == "midpoint":
        img.midpoint(1)
    elif name == "gaussian":
        img.gaussian(1)
    elif name == "laplacian":
        img.laplacian(1)
    elif name == "bilateral":
        img.bilateral(1)
    elif name == "histeq":
        img.histeq()

    img.draw_string(2, 2, name, color=255)
    img.flush()
    frame += 1
    time.sleep_ms(20)

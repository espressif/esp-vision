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
    smooth = img.copy()
    smooth.mean(2)
    img.difference(smooth)
    img.draw_string(2, 2, "difference", color=255)
    img.flush()
    time.sleep_ms(20)

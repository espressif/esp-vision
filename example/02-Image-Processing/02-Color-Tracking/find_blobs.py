# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


THRESHOLDS = [(30, 100, 15, 127, 15, 127)]

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

while True:
    img = sensor.snapshot()
    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=80, area_threshold=80, merge=True)
    for blob in blobs:
        img.draw_rectangle(blob.rect(), color=(255, 0, 0), thickness=2)
    img.flush()
    time.sleep_ms(20)

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


THRESHOLDS = [
    (30, 100, 15, 127, 15, 127),
    (20, 100, -64, -8, -32, 32),
    (0, 80, -32, 32, -128, -20),
]
COLORS = [(255, 0, 0), (0, 255, 0), (0, 0, 255)]

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

while True:
    img = sensor.snapshot()

    for i, threshold in enumerate(THRESHOLDS):
        blobs = img.find_blobs([threshold], pixels_threshold=80, area_threshold=80, merge=True)
        for blob in blobs:
            img.draw_rectangle(blob.rect(), color=COLORS[i], thickness=2)
            img.draw_cross(int(blob.cx()), int(blob.cy()), color=COLORS[i], size=8, thickness=2)

    img.flush()
    time.sleep_ms(20)

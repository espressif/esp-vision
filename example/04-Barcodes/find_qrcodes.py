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
    codes = img.find_qrcodes()
    for code in codes:
        img.draw_rectangle(code.rect(), color=255, thickness=2)
        print(code.payload())
    img.flush()
    time.sleep_ms(20)

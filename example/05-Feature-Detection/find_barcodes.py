# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

while True:
    img = sensor.snapshot()
    codes = img.find_barcodes()

    for code in codes:
        img.draw_rectangle(code.rect(), color=255, thickness=2)
        img.draw_string(code.x(), max(0, code.y() - 12), code.payload(), color=255)
        print("barcode:", code.payload(), "type:", code.type(), "rect:", code.rect())

    img.flush()
    time.sleep_ms(20)

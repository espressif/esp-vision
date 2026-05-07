# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import os
import sensor


OUT_DIR = "/sdcard"

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

img = sensor.snapshot()

for name in ("test.jpg", "test.bmp", "test.ppm"):
    path = OUT_DIR + "/" + name
    img.save(path)
    print(path, os.stat(path)[6])

print("done")

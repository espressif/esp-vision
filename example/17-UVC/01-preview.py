# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import sensor
import uvc

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)
uvc.bind("preview")
try:
    while True:
        img = sensor.snapshot()
        img.draw_string(8, 8, "ESP-VISION UVC", color=(255, 0, 0), scale=2)
        # JPEG is encoded once and independently offered to UVC and EV-MUX.
        img.flush()
finally:
    uvc.reset()

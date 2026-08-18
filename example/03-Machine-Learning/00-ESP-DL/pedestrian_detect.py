# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import espdl
import sensor
import time


MODEL = "/sdcard/pedestrian_detect/pedestrian_detect_pico.espdl"

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

det = espdl.PedestrianDetect(MODEL)

try:
    while True:
        img = sensor.snapshot()
        for x, y, w, h, score, _category in det.detect(img):
            img.draw_rectangle(x, y, w, h, color=(255, 0, 0), thickness=2)
            img.draw_string(x, max(0, y - 12), "person %.2f" % score, color=(255, 0, 0))
        img.flush()
        time.sleep_ms(20)
finally:
    det.deinit()

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import espdl
import sensor
import time


MODEL = "/sdcard/espdet_yolo11n_160_160_coco.espdl"

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

det = espdl.YOLO11(MODEL, score=0.35, nms=0.7, topk=10)

try:
    while True:
        img = sensor.snapshot()
        for x, y, w, h, score, category in det.detect(img):
            img.draw_rectangle(x, y, w, h, color=(255, 0, 0), thickness=2)
            img.draw_string(x, max(0, y - 12), "%.2f:%d" % (score, category), color=(255, 0, 0))
        img.flush()
        time.sleep_ms(20)
finally:
    det.deinit()

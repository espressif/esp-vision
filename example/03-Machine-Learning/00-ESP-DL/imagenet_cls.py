# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import espdl
import image


MODEL = "/sdcard/imagenet_cls_mobilenetv2_s8_v1.espdl"
IMAGE = "/sdcard/cat.jpg"

img = image.Image(IMAGE).to_rgb565(copy=True)
cls = espdl.ImageNetCls(MODEL, topk=5, score=0.0)

try:
    for name, score in cls.classify(img):
        print(name, "%.4f" % score)
finally:
    cls.deinit()

print("done")

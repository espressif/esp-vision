# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import espdl
import sensor
import time


MODEL = "/sdcard/espdet_yolo11n_pose_160_160_coco.espdl"

SKELETON = (
    (5, 7),
    (7, 9),
    (6, 8),
    (8, 10),
    (5, 6),
    (5, 11),
    (6, 12),
    (11, 12),
    (11, 13),
    (13, 15),
    (12, 14),
    (14, 16),
)


def valid(point):
    return point[0] > 0 and point[1] > 0


sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

pose = espdl.YOLO11nPose(MODEL, score=0.35, nms=0.7, topk=5)

try:
    while True:
        img = sensor.snapshot()
        for x, y, w, h, score, category, keypoints in pose.detect(img):
            img.draw_rectangle(x, y, w, h, color=(255, 0, 0), thickness=2)
            img.draw_string(x, max(0, y - 12), "%.2f:%d" % (score, category), color=(255, 0, 0))

            for a, b in SKELETON:
                if valid(keypoints[a]) and valid(keypoints[b]):
                    img.draw_line(
                        keypoints[a][0],
                        keypoints[a][1],
                        keypoints[b][0],
                        keypoints[b][1],
                        color=(0, 255, 0),
                        thickness=2,
                    )

            for point in keypoints:
                if valid(point):
                    img.draw_circle(point[0], point[1], 2, color=(0, 0, 255), thickness=1, fill=True)

        img.flush()
        time.sleep_ms(20)
finally:
    pose.deinit()

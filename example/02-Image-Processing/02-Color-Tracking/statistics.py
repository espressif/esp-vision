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
    stats = img.get_statistics()
    hist = img.get_histogram()
    p90 = hist.get_percentile(0.9)

    print("mean:", stats.mean(), "min:", stats.min(), "max:", stats.max(), "p90:", p90.value())
    img.draw_string(2, 2, "mean:%d" % stats.mean(), color=255)
    img.flush()
    time.sleep_ms(200)

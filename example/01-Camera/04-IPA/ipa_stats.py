# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sensor
import time


sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
if not hasattr(sensor, "get_ipa_stats") or sensor.get_ipa() is None:
    raise RuntimeError("IPA statistics are not supported by this firmware/camera")
sensor.set_ipa(True)
sensor.skip_frames(time=2000)

last_read = time.ticks_ms()
while True:
    sensor.snapshot().flush()
    if time.ticks_diff(time.ticks_ms(), last_read) < 1000:
        continue
    stats = sensor.get_ipa_stats(timeout=200)
    last_read = time.ticks_ms()
    if stats is None:
        print("No new IPA statistics within 200 ms")
        continue
    print("seq:", stats["seq"], "flags:", stats["flags"])
    print("AE [x][y]:", stats["ae"])
    print("AWB:", stats["awb"], "histogram:", stats["hist"])
    print("sharpen:", stats["sharpen"], "AF:", stats["af"])
    # stats["awb_subwin"] contains [x][y] AWB dictionaries when available.

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Stream the camera directly to UVC at the default 320x240 mode."""

import image
import sensor
import time
import uvc


def stream_camera():
    sensor.reset()
    try:
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(sensor.QVGA)
        sensor.skip_frames(time=350)
        while uvc.is_streaming():
            frame = sensor.snapshot()
            jpeg = frame.to_jpeg(quality=45, subsampling=image.JPEG_SUBSAMPLING_422, copy=True)
            if jpeg.size() > 48 * 1024:
                continue

            # Retry the same JPEG briefly if the previous frame is still in flight.
            deadline = time.ticks_add(time.ticks_ms(), 120)
            while uvc.is_streaming():
                if uvc.write(jpeg) or time.ticks_diff(deadline, time.ticks_ms()) <= 0:
                    break
                time.sleep_ms(1)
    finally:
        sensor.shutdown(True)


uvc.reset()
try:
    while True:
        if uvc.is_streaming():
            stream_camera()
        else:
            time.sleep_ms(25)
finally:
    uvc.reset()

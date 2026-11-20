# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Let the application choose a matching camera size after a host Commit."""

import sensor
import time
import uvc

# Add entries for sizes that the camera can produce and the board advertises.
CAMERA_SIZES = {
    (320, 240): sensor.QVGA,
    (640, 480): sensor.VGA,
}
requested_size = None


def on_mode(mode):
    global requested_size
    requested_size = (mode["width"], mode["height"])


def still_selected(size):
    return uvc.is_streaming() and requested_size == size


def stream_mode(size):
    framesize = CAMERA_SIZES.get(size)
    if framesize is None:
        print("No camera size configured for UVC mode:", size)
        while still_selected(size):
            time.sleep_ms(25)
        return

    sensor.reset()
    try:
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(framesize)
        sensor.skip_frames(time=350)
        uvc.bind("preview")
        while still_selected(size):
            sensor.snapshot().flush()
    finally:
        uvc.reset()
        sensor.shutdown(True)


uvc.reset()
uvc.set_mode_callback(on_mode)
try:
    while True:
        size = requested_size
        if size is not None and uvc.is_streaming():
            stream_mode(size)
        else:
            time.sleep_ms(25)
finally:
    uvc.set_mode_callback(None)
    uvc.reset()

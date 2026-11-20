# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Display a generated color chart as a fixed UVC picture."""

import image
import time
import uvc


def publish_color_chart():
    mode = uvc.get_mode()
    width, height = mode["width"], mode["height"]
    colors = (
        (255, 255, 255),
        (255, 255, 0),
        (0, 255, 255),
        (0, 255, 0),
        (255, 0, 255),
        (255, 0, 0),
        (0, 0, 255),
    )
    frame = image.Image(width, height, image.RGB565)
    for i, color in enumerate(colors):
        left = width * i // len(colors)
        right = width * (i + 1) // len(colors)
        frame.draw_rectangle((left, 0, right - left, height), color=color, fill=True)

    while uvc.is_streaming():
        if uvc.set_frame(frame):
            return  # UVC repeats its own copy until the stream closes.
        time.sleep_ms(10)


uvc.reset()
try:
    while True:
        while not uvc.is_streaming():
            time.sleep_ms(25)
        publish_color_chart()
        while uvc.is_streaming():
            time.sleep_ms(25)
finally:
    uvc.reset()

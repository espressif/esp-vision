# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Render a P4 camera frame on the LCD and leave it active for host capture."""

import display
import sensor
import time


RAINBOW = (
    (255, 0, 0),
    (255, 127, 0),
    (255, 255, 0),
    (0, 200, 0),
    (0, 100, 255),
    (75, 0, 130),
    (148, 0, 211),
)
LABEL_COLORS = (
    (255, 0, 0), (255, 127, 0), (255, 255, 0), (0, 200, 0),
    (0, 100, 255), (75, 0, 130), (148, 0, 211), (255, 0, 0),
    (255, 127, 0), (255, 255, 0),
)
lcd = None
sensor_ready = False
try:
    lcd = display.Display(backlight=100)
    sensor_ready = True
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QVGA)
    sensor.skip_frames(time=500)

    final_frame = None
    for frame_number in range(24):
        frame = sensor.snapshot()
        width = frame.width()
        height = frame.height()
        band_height = max(24, height // 8)
        for index, color in enumerate(RAINBOW):
            x0 = width * index // len(RAINBOW)
            x1 = width * (index + 1) // len(RAINBOW)
            frame.draw_rectangle(x0, 0, x1 - x0, band_height, color=color, fill=True)

        label = "esp-vision"
        label_x = (width - len(label) * 20) // 2
        label_y = max(band_height + 20, height // 3)
        frame.draw_rectangle(label_x - 8, label_y - 6, len(label) * 20 + 16, 44,
                             color=(0, 0, 0), fill=True)
        for index, char in enumerate(label):
            frame.draw_string(label_x + index * 20, label_y, char,
                              color=LABEL_COLORS[index], scale=3)
        frame.draw_rectangle(2, 2, width - 4, height - 4,
                             color=(255, 255, 255), thickness=2)
        lcd.write(frame, fit=True)
        final_frame = frame
        time.sleep_ms(20)

    if final_frame is None:
        raise RuntimeError("camera returned no frames")
    print("ESP_VISION_LCD_ACCEPTANCE_FRAME", width, height)
    print("LCD label rendered: esp-vision")
except BaseException:
    try:
        if sensor_ready:
            sensor.shutdown()
    except BaseException:
        pass
    try:
        if lcd is not None:
            lcd.deinit()
    except BaseException:
        pass
    raise

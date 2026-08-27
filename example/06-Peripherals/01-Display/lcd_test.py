# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import display
import image
import time


COLORS = (
    (255, 255, 255),
    (255, 255, 0),
    (0, 255, 255),
    (0, 255, 0),
    (255, 0, 255),
    (255, 0, 0),
    (0, 0, 255),
    (0, 0, 0),
)


lcd = display.Display(backlight=100)
frame = image.Image(lcd.width(), lcd.height(), image.RGB565)

try:
    width = frame.width()
    height = frame.height()
    bar_width = width // len(COLORS)

    for index, color in enumerate(COLORS):
        x = index * bar_width
        w = width - x if index == len(COLORS) - 1 else bar_width
        frame.draw_rectangle(x, 0, w, height, color=color, fill=True)
    frame.draw_rectangle(0, 0, width, height,
                         color=(255, 255, 255), thickness=4)
    frame.draw_line(0, 0, width - 1, height - 1,
                    color=(255, 255, 255), thickness=3)
    frame.draw_line(width - 1, 0, 0, height - 1,
                    color=(255, 255, 255), thickness=3)
    frame.draw_string(12, 12, "ESP-VISION LCD TEST",
                      color=(255, 255, 255), scale=2)
    lcd.write(frame, fit=False)
    print("LCD color bars: %dx%d" % (lcd.width(), lcd.height()))
    time.sleep_ms(3000)

    while True:
        for color in COLORS:
            frame.draw_rectangle(0, 0, width, height,
                                 color=color, fill=True)
            lcd.write(frame, fit=False)
            print("LCD solid color:", color)
            time.sleep_ms(1000)
finally:
    lcd.deinit()

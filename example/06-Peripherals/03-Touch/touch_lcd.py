# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# Draw continuous white single-touch trails on black. Press Ctrl-C to stop.
import display
import image
import time
import lcd_touch


lcd = display.Display(backlight=100)
tp = None
try:
    tp = lcd_touch.Touch()
    frame = image.Image(lcd.width(), lcd.height(), image.RGB565)
    frame.clear()
    lcd.write(frame, fit=False)
    previous_point = None

    while True:
        points = tp.read()
        # Only join single-touch samples: read() does not expose finger IDs.
        if len(points) == 1:
            x, y = points[0]
            if previous_point is not None:
                frame.draw_line(
                    previous_point[0], previous_point[1], x, y,
                    color=(255, 255, 255), thickness=6,
                )
            previous_point = points[0]
        else:
            previous_point = None
        for x, y in points:
            frame.draw_circle(x, y, 3, color=(255, 255, 255), fill=True)
        if points:
            lcd.write(frame, fit=False)
        time.sleep_ms(1)
finally:
    try:
        if tp is not None:
            tp.deinit()
    finally:
        lcd.deinit()

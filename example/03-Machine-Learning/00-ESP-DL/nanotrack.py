# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Place a target in the center box and press BOOT to start tracking.

Press BOOT again to return to selection. Lost targets keep being searched for.
Copy the three full-INT8 exports to /sdcard/nanotrack_v2/ first.
"""

from machine import Pin
import display
import espdl
import sensor
import time


MODEL_DIR = "/sdcard/nanotrack_v2"
# Active-low BOOT button on ESP32-P4X-EYE and P4 Function EV Board.
# Change this pin for other boards.
BUTTON_PIN = 35
BOX_SIZE = 64
DEBOUNCE_MS = 30
# A match score is not a calibrated target-identity probability.
MATCH_THRESHOLD = 0.95


lcd = None
tracker = None
try:
    button = Pin(BUTTON_PIN, Pin.IN, Pin.PULL_UP)
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QVGA)
    sensor.skip_frames(time=1000)
    lcd = display.Display()
    tracker = espdl.NanoTrack(
        MODEL_DIR + "/template_deploy.espdl",
        MODEL_DIR + "/search_deploy.espdl",
        MODEL_DIR + "/head_deploy.espdl",
    )
    frame = sensor.snapshot()
    width, height = frame.width(), frame.height()
    canvas = frame.copy()
    # Center at native size; smaller displays show the center crop.
    visible_w = min(width, lcd.width())
    visible_h = min(height, lcd.height())
    view = ((width - visible_w) // 2, (height - visible_h) // 2,
            visible_w, visible_h)
    offset_x = (lcd.width() - visible_w) // 2
    offset_y = (lcd.height() - visible_h) // 2
    size = min(BOX_SIZE, visible_w, visible_h)
    roi = ((width - size) // 2, (height - size) // 2, size, size)
    tracking = False
    last_button = stable_button = button.value()
    changed_at = time.ticks_ms()
    # A button held at startup must be released before it can confirm.
    armed = stable_button == 1

    while True:
        now = time.ticks_ms()
        level = button.value()
        pressed = False
        if level != last_button:
            last_button = level
            changed_at = now
        if level != stable_button and time.ticks_diff(now, changed_at) >= DEBOUNCE_MS:
            stable_button = level
            if level:
                armed = True
            elif armed:
                armed = False
                pressed = True

        if pressed:
            if tracking:
                tracking = False
            else:
                canvas.draw_image(frame, 0, 0)
                canvas.draw_string(view[0] + 2, view[1] + 2, "Initializing...",
                                   color=(255, 255, 0))
                lcd.write(canvas, x=offset_x, y=offset_y, roi=view, fit=False)
                # Use the last displayed raw frame, without any annotations.
                tracker.init(frame, roi)
                tracking = True

        frame = sensor.snapshot()
        begin = time.ticks_ms()
        result = tracker.track(frame, threshold=MATCH_THRESHOLD) if tracking else None
        canvas.draw_image(frame, 0, 0)
        if result is not None:
            x, y, w, h, score = result
            message = "Tracking - BOOT: select"
            canvas.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
            canvas.draw_string(view[0] + 2, view[1] + visible_h - 14,
                               "match %.2f  %d ms" % (score, time.ticks_diff(time.ticks_ms(), begin)),
                               color=(0, 255, 0))
        elif tracking:
            # Preserve the template and retry near the last accepted position.
            message = "Lost - searching..."
        else:
            message = "Aim box, press BOOT"
            canvas.draw_rectangle(roi, color=(255, 255, 0), thickness=2)
        canvas.draw_string(view[0] + 2, view[1] + 2, message, color=(255, 255, 255))
        lcd.write(canvas, x=offset_x, y=offset_y, roi=view, fit=False)
        time.sleep_ms(0 if tracking else 10)
finally:
    try:
        if tracker is not None:
            tracker.deinit()
    finally:
        if lcd is not None:
            lcd.deinit()

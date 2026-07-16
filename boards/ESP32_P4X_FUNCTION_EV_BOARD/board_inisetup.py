# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

MAIN_PY = """\
import display
import sensor
import time

print("ESP-VISION ESP32_P4X_FUNCTION_EV_BOARD ready")

lcd = display.Display()

try:
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QVGA)
    sensor.skip_frames(time=1000)

    while True:
        img = sensor.snapshot()
        lcd.write(img)
        time.sleep_ms(20)
finally:
    sensor.shutdown()
    lcd.deinit()
"""

README_TXT = """\
ESP-VISION ESP32_P4X_FUNCTION_EV_BOARD

Edit main.py to run your Python vision script.
Use the ESP-VISION VSCode extension to run scripts and preview frames.
The default main.py previews the camera image on the board LCD.
"""

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

MAIN_PY = """\
from machine import Pin
from neopixel import NeoPixel
import time

print("ESP32_P4X_VISION ready")

led = NeoPixel(Pin(9), 1)

try:
    while True:
        led[0] = (0, 8, 0)
        led.write()
        time.sleep_ms(500)
        led[0] = (0, 0, 0)
        led.write()
        time.sleep_ms(500)
finally:
    led[0] = (0, 0, 0)
    led.write()
"""

README_TXT = """\
ESP32_P4X_VISION

Edit main.py to run your Python vision script.
The default main.py keeps the board idle so host tools can take control.
"""

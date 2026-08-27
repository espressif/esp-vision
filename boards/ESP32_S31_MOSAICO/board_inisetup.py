# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

MAIN_PY = """\
import display
import sensor
import time

print("ESP-VISION ESP32_S31_MOSAICO ready")

lcd = display.Display()
CROP = 480

try:
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    # VGA is 480x640 after the board's 90-degree PPA rotation.
    sensor.set_framesize(sensor.VGA)
    sensor.skip_frames(time=1000)

    while True:
        img = sensor.snapshot()
        x = (img.width() - CROP) // 2
        y = (img.height() - CROP) // 2
        lcd.write(img, roi=(x, y, CROP, CROP), fit=True)
        time.sleep_ms(20)
finally:
    sensor.shutdown()
    lcd.deinit()
"""

README_TXT = """\
ESP-VISION ESP32_S31_MOSAICO

Edit main.py to run your Python vision script.
Use the ESP-VISION VSCode extension to run scripts and preview frames.
The default main.py previews a center 480x480 crop of the OV3640 VGA frame on the 480x480 CO5300 display.
The board uses onboard NAND Flash instead of an SD card; NAND storage is not yet exposed by this firmware.
"""

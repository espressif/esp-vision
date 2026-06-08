# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

MAIN_PY = """\
import time

print("ESP-VISION ESP32_S31_KORVO ready")

while True:
    time.sleep_ms(1000)
"""

README_TXT = """\
ESP-VISION ESP32_S31_KORVO

Edit main.py to run your Python vision script.
Use the ESP-VISION VSCode extension to run scripts and preview frames.
The default main.py keeps the board idle so host tools can take control.
"""

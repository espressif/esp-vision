# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

MAIN_PY = """\
import time

print("ESP-VISION board ready")

while True:
    time.sleep_ms(1000)
"""

README_TXT = """\
ESP-VISION board template

Edit main.py to run your Python vision script.
Use this board directory as the starting point for a new ESP-VISION board.
The default main.py keeps the board idle so host tools can take control.
"""

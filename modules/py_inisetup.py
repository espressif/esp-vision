# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import os


_MARKER = "/.esp_vision_disk"

_DEFAULT_MAIN_PY = """\
import time

print("ESP-VISION ready")

while True:
    time.sleep_ms(1000)
"""

_DEFAULT_README_TXT = """\
ESP-VISION

Edit main.py to run your Python vision script.
Use the ESP-VISION VSCode extension to run scripts and preview frames.
The default main.py keeps the board idle so host tools can take control.
"""


def _board_default(name, default):
    try:
        import board_inisetup
        return getattr(board_inisetup, name, default)
    except (ImportError, AttributeError):
        return default


def _exists(path):
    try:
        os.stat(path)
        return True
    except OSError:
        return False


def _write_if_missing(path, data):
    if _exists(path):
        return
    with open(path, "w") as f:
        f.write(data)


def setup():
    if _exists(_MARKER):
        return

    _write_if_missing("/main.py", _board_default("MAIN_PY", _DEFAULT_MAIN_PY))
    _write_if_missing("/README.txt", _board_default("README_TXT", _DEFAULT_README_TXT))
    _write_if_missing(_MARKER, "ESP-VISION flash filesystem\n")


try:
    setup()
except OSError:
    pass

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Log the mode selected by the UVC host after a successful Commit."""

import time
import uvc


def on_mode(mode):
    print("UVC mode:", mode["width"], mode["height"], mode["interval_us"])


uvc.set_mode_callback(on_mode)
try:
    while True:
        time.sleep_ms(1000)
finally:
    # This unregisters the callback; uvc.reset() only resets the frame source.
    uvc.set_mode_callback(None)

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations


#: Touchscreen object for the board LCD.
#: Requires a board with configured touch hardware; otherwise raises OSError.
class Touch:
    #: Initialize touch input without requiring display initialization.
    #: A new instance takes ownership of the shared touch device.
    def __init__(self) -> None: ...
    def __del__(self) -> None: ...
    #: Return (x, y) pixel coordinates for each touch, or an empty tuple.
    #: Raise OSError if this instance is inactive or reading fails.
    def read(self) -> tuple[tuple[int, int], ...]: ...
    #: Release the touch device if this instance still owns it.
    def deinit(self) -> None: ...

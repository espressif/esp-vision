# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from typing import Sequence

from image import Image

#: Region of interest tuple: (x, y, w, h).
ROI = tuple[int, int, int, int] | Sequence[int]


#: ESP32 display object for the board LCD.
class ESP32Display:
    #: Create and initialize the display.
    #: width: requested display width; 0 uses board default.
    #: height: requested display height; 0 uses board default.
    #: refresh: target refresh rate in Hz.
    #: backlight: initial backlight percentage, from 0 to 100.
    def __init__(self, width: int = 0, height: int = 0, refresh: int = 60, *, backlight: int = 100) -> None: ...
    def __del__(self) -> None: ...
    #: Release the display driver resources.
    def deinit(self) -> None: ...
    #: Return the physical display width in pixels.
    def width(self) -> int: ...
    #: Return the physical display height in pixels.
    def height(self) -> int: ...
    #: Clear the display.
    #: display_off: True may turn the panel output off when supported by the board.
    def clear(self, display_off: bool = False) -> None: ...
    #: Get or set backlight brightness.
    #: value: None returns current brightness; otherwise set 0 to 100 percent.
    def backlight(self, value: int | None = None) -> int | None: ...
    #: Draw an image on the display.
    #: image: source image, normally from sensor.snapshot().
    #: x, y: destination top-left position in display pixels.
    #: x_scale, y_scale: optional manual scale factors.
    #: roi: optional source rectangle (x, y, w, h).
    #: fit: True scales the image to fit the display area.
    def write(
        self,
        image: Image,
        *,
        x: int = 0,
        y: int = 0,
        x_scale: float | None = None,
        y_scale: float | None = None,
        roi: ROI | None = None,
        fit: bool = True,
    ) -> None: ...


Display = ESP32Display

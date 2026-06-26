# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from typing import Any, TypedDict

from image import Image

#: Pixel format constants accepted by set_pixformat().
GRAYSCALE: int
RGB565: int
#: Frame size constants accepted by set_framesize().
QQVGA: int
QVGA: int
VGA: int


#: Dictionary returned by status().
class SensorStatus(TypedDict):
    ready: bool
    id: int
    width: int
    height: int
    pixformat: int
    hmirror: bool
    vflip: bool
    raw_width: int
    raw_height: int
    active_width: int
    active_height: int


#: Reset and initialize the camera with the board default sensor configuration.
def reset() -> None: ...
#: Stop or restart the camera stream.
#: enable: True shuts the camera down; False starts it again.
def shutdown(enable: bool = True) -> None: ...
#: Select the output pixel format.
#: pixformat: sensor.GRAYSCALE or sensor.RGB565.
def set_pixformat(pixformat: int) -> None: ...
#: Return the current pixel format constant.
def get_pixformat() -> int: ...
#: Select the output frame size.
#: framesize: sensor.QQVGA, sensor.QVGA, or sensor.VGA when supported by the board.
def set_framesize(framesize: int) -> None: ...
#: Return the current frame size constant.
def get_framesize() -> int: ...
#: Return the current output image width in pixels.
def width() -> int: ...
#: Return the current output image height in pixels.
def height() -> int: ...
#: Return the camera sensor ID.
def get_id() -> int: ...
#: Mirror the camera image horizontally.
#: enable: True enables horizontal mirror.
def set_hmirror(enable: bool) -> None: ...
#: Return whether horizontal mirror is enabled.
def get_hmirror() -> bool: ...
#: Flip the camera image vertically.
#: enable: True enables vertical flip.
def set_vflip(enable: bool) -> None: ...
#: Return whether vertical flip is enabled.
def get_vflip() -> bool: ...
#: Drop frames while camera exposure and processing settle.
#: time: milliseconds to wait. n: number of frames to skip.
def skip_frames(time: int = 0, n: int = 0) -> None: ...
#: Capture one image from the camera.
#: buffer: reserved for compatibility; pass None in current ESP-VISION builds.
def snapshot(buffer: Any | None = None) -> Image: ...
#: Return camera readiness, size, format, mirror, flip, and crop status.
def status() -> SensorStatus: ...

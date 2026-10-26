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


class IPAAWBStats(TypedDict):
    counted: int
    sum_r: int
    sum_g: int
    sum_b: int


class IPAAFStats(TypedDict):
    definition: int
    luminance: int


class IPAStats(TypedDict):
    seq: int
    flags: int
    ae: list[list[int]] | None
    awb: IPAAWBStats | None
    awb_subwin: list[list[IPAAWBStats]] | None
    hist: list[int] | None
    sharpen: int | None
    af: list[IPAAFStats] | None


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
#: Optional API: present only in firmware with IPA/ISP control enabled.
#: Start or stop the IPA controller after camera initialization; briefly pauses capture.
#: Existing ISP tuning is retained when disabled; enabling returns configured blocks to IPA.
#: Sensor-side automatic exposure/white balance is independent. Unsupported cameras raise OSError.
def set_ipa(enable: bool) -> None: ...
#: Optional API: present only in firmware with IPA/ISP control enabled.
#: Return the IPA controller state, or None when runtime control is unsupported.
#: A supported camera must be initialized first.
def get_ipa() -> bool | None: ...
#: Optional API: collect one new statistics packet while IPA is running.
#: timeout: 1..1000 milliseconds; None means timeout or unsupported backend.
#: Uninitialized/stopped cameras or disabled IPA raise OSError.
#: AE and AWB subwindow grids use [x][y]; unavailable blocks are None.
def get_ipa_stats(timeout: int = 1000) -> IPAStats | None: ...
#: Optional API: present only in firmware with IPA/ISP control enabled.
#: Read current ISP block parameters, or None if unsupported. Initialize the camera first.
#: Blocks the camera driver or chip revision does not implement return None,
#: including "exposure" on sensors whose driver makes it write-only.
#: Readback is driver configuration, not a frame commit barrier or sensor-side AWB gains.
#: OV2710 does not support manual exposure or pixel_gain reads or writes.
def get_isp(block: str) -> dict[str, Any] | None: ...
#: Optional API: present only in firmware with IPA/ISP control enabled.
#: Update one ISP block using keyword parameters; unspecified fields retain their values.
#: Disable IPA before writing. Invalid parameters raise ValueError; device errors raise OSError.
#: BF level is 2..20; use enable=False to disable filtering.
#: Consume queued frames before evaluating a change; skip_frames is not a hardware commit barrier.
def set_isp(block: str, **params: Any) -> None: ...
#: Drop frames while camera exposure and processing settle.
#: time: milliseconds to wait. n: number of frames to skip.
def skip_frames(time: int = 0, n: int = 0) -> None: ...
#: Capture one image from the camera.
#: buffer: optional writable buffer of at least the configured frame size. The returned Image retains it.
def snapshot(buffer: Any | None = None, /) -> Image: ...
#: Return camera readiness, size, format, mirror, flip, and crop status.
def status() -> SensorStatus: ...

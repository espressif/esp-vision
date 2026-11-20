# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Native USB webcam output where the board build enables UVC.

The released fallback advertises MJPEG 320x240 at 10 FPS. Every UVC build
accepts source image dimensions that differ from the advertised mode at
runtime, but host compatibility is not guaranteed. Opt-in candidate mode
tables are disabled by default and keep 320x240 at 10 FPS first. Mosaico adds
480x480 and 640x480 candidates at 10, 20 and 30 FPS; these sizes and rates are
not validated sensor modes. JPEG frame slots remain capped at 48 KiB each.
"""
from typing import Callable
from image import Image

def write(img: Image) -> bool:
    """Publish an image; runtime accepts differing dimensions without host compatibility guarantees."""
    ...

def set_frame(img: Image) -> bool:
    """Set a fixed image; runtime accepts differing dimensions without host compatibility guarantees."""
    ...

def bind(source: str | None) -> None:
    """Bind 'preview' to Image.flush(); None detaches and restores the logo."""
    ...

def reset() -> None:
    """Detach the source and restore the built-in ESP-VISION logo."""
    ...

def is_streaming() -> bool:
    """Whether the host has enabled the UVC streaming interface."""
    ...

def get_mode() -> dict:
    """Return the host-selected mode; the default fallback is MJPEG 320x240 at 10 FPS."""
    ...

def set_mode_callback(callback: Callable[[dict], object] | None) -> None:
    """Notify on successful host Commit (replaying the latest mode once if present); None unregisters."""
    ...

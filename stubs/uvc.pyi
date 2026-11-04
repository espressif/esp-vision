# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Native USB webcam output on boards with USB-OTG device support."""
from image import Image

def write(img: Image) -> bool:
    """Publish a QVGA frame; return False when the host is not streaming."""
    ...

def set_frame(img: Image) -> bool:
    """Copy a QVGA fixed frame, detach preview and repeat until replaced/reset."""
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
    """Return format, width, height and interval_us for the supported mode."""
    ...

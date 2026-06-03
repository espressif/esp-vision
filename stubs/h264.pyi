# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from image import Image


class H264Encoder:
    def __init__(
        self,
        width: int,
        height: int,
        *,
        fps: int = 15,
        gop: int = 0,
        bitrate: int = 0,
        qp_min: int = 25,
        qp_max: int = 45,
    ) -> None: ...
    def encode(self, image: Image) -> bytes: ...
    def keyframe(self) -> bool: ...
    def close(self) -> None: ...
    def __del__(self) -> None: ...

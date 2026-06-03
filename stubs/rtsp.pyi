# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations


class RTSPServer:
    def __init__(
        self,
        width: int,
        height: int,
        *,
        fps: int = 15,
        listen_port: int = 8554,
        max_frame_len: int = 0,
    ) -> None: ...
    def send(self, nal: bytes) -> None: ...
    def stop(self) -> None: ...
    def __del__(self) -> None: ...

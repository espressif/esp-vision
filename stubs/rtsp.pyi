# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations


#: Minimal RTSP server that streams H.264 video over the network.
#: Create it once Wi-Fi is up, then push encoded NAL units from an
#: h264.H264Encoder with send(). Clients connect to
#: ``rtsp://<board-ip>:<listen_port>/``. Only video (no audio) is served.
class RTSPServer:
    #: Start the RTSP server and advertise the given stream format.
    #: width: advertised video width in pixels.
    #: height: advertised video height in pixels.
    #: fps: advertised frame rate, used for SDP and pacing.
    #: listen_port: TCP port the RTSP service binds to.
    #: max_frame_len: max accepted encoded frame size in bytes; 0 uses width*height as a safe ceiling.
    def __init__(
        self,
        width: int,
        height: int,
        *,
        fps: int = 15,
        listen_port: int = 8554,
        max_frame_len: int = 0,
    ) -> None: ...
    #: Queue one encoded H.264 frame (Annex-B NAL units) for delivery.
    #: Frames are sent in order; if the client is slow or absent the frame is
    #: dropped rather than blocking the caller. Frames larger than
    #: max_frame_len are ignored.
    #: nal: encoded frame bytes from h264.H264Encoder.encode().
    def send(self, nal: bytes) -> None: ...
    #: Stop the server, join its task, and free queued frames.
    def stop(self) -> None: ...
    def __del__(self) -> None: ...

# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from image import Image


#: Hardware-accelerated H.264 video encoder.
#: Feed it images frame by frame with encode(); it returns Annex-B NAL units
#: ready to mux into a file or stream over the network (see the rtsp module).
class H264Encoder:
    #: Open an encoder for a fixed frame size.
    #: width: frame width in pixels.
    #: height: frame height in pixels.
    #: fps: target frame rate; also the default GOP length.
    #: gop: keyframe (IDR) interval in frames; 0 selects one keyframe per second (== fps).
    #: bitrate: target bitrate in bits per second; 0 auto-selects width*height*fps.
    #: qp_min: lower quantization-parameter bound (better quality, larger frames).
    #: qp_max: upper quantization-parameter bound (lower quality, smaller frames).
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
    #: Encode one image and return its Annex-B NAL units as bytes.
    #: image: source frame; its size must match the encoder width/height.
    def encode(self, image: Image) -> bytes: ...
    #: Return True if the most recently encoded frame was a keyframe (IDR/I).
    def keyframe(self) -> bool: ...
    #: Release the encoder. Using the object afterwards raises OSError.
    def close(self) -> None: ...
    def __del__(self) -> None: ...

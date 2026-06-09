# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from typing import Literal, overload

from image import Image

#: Stream backed by a file on storage (e.g. ``/sdcard/stream.bin``).
FILE_STREAM: int
#: Stream backed by a fixed-size buffer in PSRAM/RAM.
MEMORY_STREAM: int

#: Image descriptor tuple used to size a memory stream: (width, height, pixformat).
ImageInfo = tuple[int, int, int]


#: Record and replay sequences of images, preserving inter-frame timing.
#: Exposed as ``image.ImageIO``. A file stream reads/writes the OpenMV
#: "OMV IMG STR" container on storage; a memory stream keeps frames in a
#: pre-allocated buffer for fast capture/playback.
class ImageIO:
    #: Open a file stream for reading ("r") or writing ("w").
    #: stream: path to the stream file.
    #: mode: "r" to read, "w" to create/overwrite.
    @overload
    def __init__(self, stream: str, mode: Literal["r", "w"]) -> None: ...
    #: Allocate a memory stream for count frames of the given image format.
    #: stream: (width, height, pixformat) descriptor of each frame.
    #: count: number of frames the buffer can hold.
    @overload
    def __init__(self, stream: ImageInfo, count: int) -> None: ...
    def __del__(self) -> None: ...
    #: Return the stream type: FILE_STREAM or MEMORY_STREAM.
    def type(self) -> int: ...
    #: Return True if the stream has been closed.
    def is_closed(self) -> bool: ...
    #: Return the number of frames recorded in the stream.
    def count(self) -> int: ...
    #: Return the current frame index (read/write cursor).
    def offset(self) -> int: ...
    #: Return the container version for file streams, or None for memory streams.
    def version(self) -> int | None: ...
    #: Return the per-frame buffer size for memory streams, or None for file streams.
    def buffer_size(self) -> int | None: ...
    #: Return the total stream size in bytes.
    def size(self) -> int: ...
    #: Append one image to the stream (records the elapsed time since the last write).
    #: image: frame to store.
    def write(self, image: Image) -> ImageIO: ...
    #: Read the next frame from the stream.
    #: copy_to_fb: True loads the frame into the frame buffer (and updates the preview).
    #: loop: True rewinds to the first frame at end-of-stream instead of returning None.
    #: pause: True sleeps to honor the frame's recorded timestamp (real-time playback).
    def read(self, copy_to_fb: bool = True, *, loop: bool = True, pause: bool = True) -> Image | None: ...
    #: Move the read/write cursor to the given frame index.
    #: offset: target frame index.
    def seek(self, offset: int) -> ImageIO: ...
    #: Flush buffered file data to storage (no-op for memory streams).
    def sync(self) -> ImageIO: ...
    #: Close the stream and release its resources.
    def close(self) -> None: ...

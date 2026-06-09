The Image Model
===============

:link_to_translation:`zh_CN:[中文]`

Every vision operation in ESP-VISION reads or writes an :py:class:`image.Image`:
a rectangular grid of pixels plus a small header describing its width, height,
and pixel format. Understanding how those pixels are laid out, which color
spaces the algorithms use, and how the underlying memory is managed is the key
to writing fast, correct scripts.

Pixel formats
-------------

The pixel format determines how many bytes each pixel occupies and how its value
is interpreted.

.. list-table::
   :header-rows: 1
   :widths: 18 14 68

   * - Format
     - Bytes/pixel
     - Description
   * - ``BINARY``
     - 1 bit
     - Black/white. Used for masks and the output of :py:meth:`image.Image.binary`.
   * - ``GRAYSCALE``
     - 1
     - 8-bit intensity (0-255). The format most filters and detectors prefer.
   * - ``RGB565``
     - 2
     - 16-bit color: 5 bits red, 6 bits green, 5 bits blue.
   * - ``BAYER``
     - 1
     - Raw single-channel mosaic straight from the sensor; debayered on demand.
   * - ``YUV422``
     - 2
     - Luminance/chrominance packing, common in camera/codec pipelines.
   * - ``JPEG`` / ``PNG``
     - varies
     - Compressed byte streams produced by :py:meth:`image.Image.to_jpeg` / ``to_png``.

RGB565 packs a color into 16 bits, trading color precision for half the memory
of RGB888. Green gets the extra bit because the human eye is most sensitive to
it. :py:meth:`image.Image.get_pixel` can return either the packed value or an
``(r, g, b)`` tuple expanded back to 8 bits per channel.

Color spaces
------------

Although pixels are *stored* as grayscale or RGB565, color thresholding works in
the **LAB** color space. LAB separates lightness (L) from two color-opponent
axes (A: green-red, B: blue-yellow), so a threshold expressed in LAB is far more
robust to brightness changes than one in RGB. This is why a color threshold is a
six-tuple ``(l_min, l_max, a_min, a_max, b_min, b_max)`` while a grayscale
threshold is just ``(min, max)``.

The module-level conversion helpers (:py:func:`image.rgb_to_lab`,
:py:func:`image.lab_to_rgb`, :py:func:`image.rgb_to_grayscale`, and the rest of
the ``*_to_*`` family) expose these conversions for a single pixel, which is
handy when computing thresholds offline.

The frame buffer and ``fb_alloc``
---------------------------------

Embedded memory is scarce, so ESP-VISION avoids per-frame heap allocation.
:py:func:`sensor.snapshot` returns an image backed by a reusable **frame
buffer**: the next ``snapshot()`` overwrites it. If you need to keep a frame
around (for example, to compare against a later frame in
:py:meth:`image.Image.difference`), make an explicit ``img.copy()``.

Many algorithms need scratch memory that lives only for the duration of a call.
These use a stack-like allocator, ``fb_alloc``, that carves temporary buffers
out of the frame-buffer region and frees them all at once when the operation
returns. This is why heavy methods do not fragment the heap, and why you should
prefer reusing one image over allocating new ones in a hot loop.

Region of interest (ROI)
------------------------

Most analysis and conversion methods accept an ``roi=(x, y, w, h)`` keyword that
restricts the operation to a sub-rectangle of the image. Working on an ROI is
both faster (fewer pixels) and more selective (ignore irrelevant areas). The ROI
is always expressed in the source image's pixel coordinates.

The Camera Pipeline
===================

:link_to_translation:`zh_CN:[中文]`

:py:func:`sensor.snapshot` looks like a single call, but behind it sits a
hardware-specific capture pipeline that turns photons into an
:py:class:`image.Image`. Knowing the stages helps explain the configuration
calls (``set_pixformat``, ``set_framesize``, ``skip_frames``) and the
performance characteristics of each board.

From sensor to image
--------------------

#. **Configuration.** :py:func:`sensor.reset` brings up the image sensor over
   its control bus and applies the board default configuration.
   :py:func:`sensor.set_pixformat` and :py:func:`sensor.set_framesize` select the
   output format and resolution.
#. **Streaming.** The sensor pushes pixels over a camera interface (MIPI-CSI or
   DVP) into a DMA-fed frame buffer in PSRAM. Capture is double-buffered so the
   next frame fills while the current one is processed.
#. **Conversion.** The raw sensor data (often Bayer or YUV) is converted to the
   requested pixel format. On ESP32-P4 a hardware **PPA** (Pixel Processing
   Accelerator) performs scaling and color conversion; on ESP32-S3 this is done
   in software.
#. **Settling.** Auto-exposure and auto-white-balance need a few frames to
   converge, which is why examples call ``sensor.skip_frames(time=2000)`` after
   configuration.
#. **Snapshot.** :py:func:`sensor.snapshot` hands back the latest frame as an
   :py:class:`image.Image` that shares the frame-buffer memory (see
   :doc:`image-model`).

Per-board backends
------------------

The camera backend is selected per board at build time:

.. list-table::
   :header-rows: 1
   :widths: 22 30 48

   * - Board family
     - Backend
     - Notes
   * - ESP32-P4
     - ``esp_video`` / V4L2 + PPA
     - MIPI-CSI sensors; hardware-accelerated scaling and color conversion.
   * - ESP32-S3
     - ``esp32-camera``
     - DVP sensors; conversion in software, so prefer smaller frame sizes.

Because the public ``sensor`` API is identical across boards, the same script
runs on both; only the achievable resolution and frame rate differ.

Frame sizes
-----------

``set_framesize`` accepts named sizes (``QQVGA``, ``QVGA``, ``VGA``, ...) drawn
from a shared framesize table. Smaller frames cost less memory and less
processing time per stage, so when a model or algorithm only needs a small
input, capture small rather than capturing large and downscaling.

Examples
========

:link_to_translation:`zh_CN:[中文]`

ESP-VISION ships runnable MicroPython scripts under ``example/``. Copy a script
to the board or run it from the host tool. The folders are organized by topic.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Folder
     - Contents
   * - ``00-HelloWorld``
     - Minimal first script (``helloworld.py``).
   * - ``01-Camera``
     - Camera capture and streaming examples, starting with ``00-Snapshot``.
   * - ``02-Image-Processing``
     - Drawing, filters, color tracking, and frame differencing.
   * - ``03-Machine-Learning``
     - ESP-DL inference: ESPDet, YOLO11, YOLO11n pose, ImageNet
       classification (``00-ESP-DL``).
   * - ``04-Barcodes``
     - QR code detection (``find_qrcodes.py``).
   * - ``05-Feature-Detection``
     - AprilTags, lines, and circles.
   * - ``06-Peripherals``
     - Storage (SD card), display/preview, and Wi-Fi (WebREPL).

.. only:: esp32p4

   The ESP32-P4 build also supports ``01-Camera/01-H264``,
   ``01-Camera/02-RTSP``, and the 1D barcode examples under
   ``05-Feature-Detection``.

Wi-Fi MJPEG Streaming
---------------------

``01-Camera/03-MJPEG/wifi_mjpeg_stream.py`` captures JPEG frames and serves
them as an HTTP MJPEG stream. Set ``SSID`` and ``PASSWORD`` in the script, run
it on a camera board whose firmware exposes ``network.WLAN``, then open the
printed ``http://<board-ip>/`` address in a browser.

This is a browser-friendly MJPEG stream, not RTSP. ``JPEG_QUALITY`` and
``FRAME_DELAY_MS`` control the image quality and frame interval. Availability
depends on the board's camera and networking hardware and firmware
configuration.

Mapping to the API Reference
----------------------------

- ``01-Camera`` and ``06-Peripherals/01-Display`` use :doc:`../api-reference/sensor`
  and :doc:`../api-reference/display`.
- ``01-Camera/03-MJPEG`` also uses the standard MicroPython ``network`` and
  ``socket`` modules.
- ``02-Image-Processing``, ``04-Barcodes``, and ``05-Feature-Detection`` use
  :doc:`../api-reference/image`.
- ``03-Machine-Learning`` uses :doc:`../api-reference/espdl`.

.. only:: esp32p4

   - ``01-Camera/01-H264`` and ``01-Camera/02-RTSP`` use
     :doc:`../api-reference/h264` and :doc:`../api-reference/rtsp`.

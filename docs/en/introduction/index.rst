Introduction
============

:link_to_translation:`zh_CN:[中文]`

What Is ESP-VISION
------------------

ESP-VISION is a MicroPython vision runtime for the ESP32 platform. It builds a
custom firmware from the upstream MicroPython ESP32 port plus an ESP-VISION
overlay, a set of C modules exposed to Python, a self-written platform layer,
and OpenMV's ``imlib`` as a vision component.

Key Features
------------

- ``sensor``, ``image``, and ``display`` Python APIs for camera capture, image
  processing, preview, and LCD output.
- Common board runtime services for camera, display, storage, USB preview,
  JPEG, and board-specific peripherals.
- Vision algorithms based in part on OpenMV ``imlib``, covering drawing,
  filtering, color tracking, feature detection, QR code, barcode, and AprilTag
  workflows.
- ESP-DL model inference helpers for object detection, pose detection, and
  image classification.
- Development through a VSCode-based host tool or Web IDE, with firmware builds
  available through ``make`` or ``idf.py``.

Supported Boards
----------------

ESP-VISION targets ESP32-P4, ESP32-S3, and ESP32-S31 boards. The supported board packages
are ``ESP32_P4X_EYE``, ``ESP32_P4X_FUNCTION_EV_BOARD``, ``ESP32_S3_EYE``, and
``ESP32_S31_KORVO``. ``TEMPLATE`` is provided for new-board bring-up. See
:doc:`../target-support/index` for target-specific modules and constraints.

See :doc:`../get-started/index` to build and flash the firmware, and
:doc:`../architecture/index` for how the pieces fit together.

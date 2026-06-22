Introduction
============

:link_to_translation:`zh_CN:[中文]`

What Is ESP-VISION
------------------

ESP-VISION is a Low-Code Edge AI & Computer Vision Framework for Espressif SoCs. It deeply integrates essential capabilities including camera capture, image processing, video encoding and decoding, network transmission, model deployment, and AI inference, while providing unified and standardized Python APIs that enable developers to rapidly build edge applications combining visual capture, intelligent recognition, display, and media streaming.

Key Features
------------

- Unified camera, image, display, video encoding, preview, and streaming APIs across supported chips and boards.
- Image processing capabilities covering drawing, filtering, color tracking, feature detection, QR codes, barcodes, and AprilTags.
- ESP-DL-powered object detection, pose estimation, and image classification, plus TensorFlow Lite Micro support for ``.tflite`` model execution.
- Efficient C/C++ foundation components work closely with on-chip multimedia peripherals and hardware acceleration modules to deliver high-performance, real-time application execution.
- Development through a VSCode-based host tool or Web IDE, with firmware builds managed through ``idf.py``.

Supported Boards
----------------

ESP-VISION supports boards based on ESP32-P4, ESP32-S3, and ESP32-S31. See :doc:`../target-support/index` for the full board list and the chip-specific modules and constraints.

See :doc:`../get-started/index` to build and flash the firmware, and :doc:`../architecture/index` for how the pieces fit together.

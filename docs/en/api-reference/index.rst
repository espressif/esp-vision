API Reference
=============

:link_to_translation:`zh_CN:[中文]`

ESP-VISION provides Python APIs for camera capture, image processing, display, video encoding and streaming, and AI inference. Developers can also directly use the standard MicroPython APIs enabled in the firmware for hardware control, networking, file access, system services, and general application development. ESP-VISION maintains compatibility with commonly used OpenMV module names where applicable, including ``sensor`` and ``image``, and additionally provides modules such as ``display``, ``espdl``, and ``tflite``.

The ESP-VISION module pages are kept in sync with the type stubs under ``stubs/``, which are also usable for IDE completion. Their availability is derived from ``micropython.cmake`` during the documentation build. The :doc:`micropython` page indexes the language, standard-library, hardware, networking, filesystem, and runtime APIs that ESP-VISION firmware inherits from the pinned MicroPython version.

.. toctree::
   :maxdepth: 1

   micropython

.. include:: _generated/module-toctrees.rst

API Reference
=============

:link_to_translation:`zh_CN:[中文]`

ESP-VISION exposes its functionality through C-implemented MicroPython modules.
The public module names stay OpenMV-compatible: the camera module is ``sensor``,
with ``image``, ``display``, and ``espdl`` alongside it. The codec stream type
is exposed as ``image.ImageIO``. This page only lists modules compiled for the
selected target.

The pages below are the reference for each module. They are kept in sync with the
type stubs under ``stubs/``, which are also usable for IDE completion. Module
availability is derived from ``micropython.cmake`` during the documentation
build.

This guide covers ESP-VISION modules. Standard MicroPython modules are selected
by ``overlay/micropython/ports/esp32/mpconfigport.h``, board-level
``mpconfigboard.h`` overrides, ESP-IDF version checks, and SoC capability
macros. Their availability therefore cannot always be determined from target
alone.

.. include:: _generated/module-toctrees.rst

Project Relationships
=====================

:link_to_translation:`zh_CN:[中文]`

This chapter explains how ESP-VISION relates to MicroPython, OpenMV, and its major third-party components. For licensing and redistribution obligations, see :doc:`../license/index`.

ESP-VISION, MicroPython, and OpenMV
----------------------------------------

The relationship can be summarized as follows:

- **MicroPython provides the language runtime and firmware base.** ESP-VISION starts from a pinned MicroPython ESP32 port and adds ``overlay/micropython/``, MicroPython user C modules, and ESP-IDF components. It does not implement a separate Python interpreter.
- **OpenMV is an upstream source for part of the vision implementation and API design.** ESP-VISION reuses a subset of OpenMV ``imlib`` algorithms and some ``image`` binding and helper code, preserving their original license and copyright notices.
- **ESP-VISION is an independent integration for Espressif chips.** Camera, display, storage, USB, codec, ESP-DL inference, TFLite Micro inference, and board support are integrated using ESP-VISION and ESP-IDF components. ESP-VISION is not a fork of the OpenMV firmware and does not include the complete OpenMV hardware abstraction, IDE, or feature set.

The ``sensor`` and ``image`` modules therefore follow the OpenMV API style to make script migration easier, but **matching API names do not imply complete behavioral or feature compatibility**. Support also depends on the chip, board, memory, peripherals, and build options. Use this guide's :doc:`../api-reference/index` as the authoritative support reference.

Dependency Layers
-----------------

The main code layers in an ESP-VISION firmware image are:

.. code-block:: text

   User scripts
      |
      +-- MicroPython runtime and ESP32 port
      |
      +-- ESP-VISION Python modules
      |      +-- OpenMV-style sensor / image APIs
      |      +-- display / imageio / espdl / tflite / h264 / rtsp
      |
      +-- Algorithms and middleware
      |      +-- OpenMV imlib subset
      |      +-- ulab / ZXing-C++ / ESP-DL / TFLite Micro
      |
      +-- ESP-IDF, managed components, and board backends

Dependencies come from three locations:

- Git submodules under ``lib/``, such as MicroPython, ``ulab``, and ZXing-C++;
- third-party-derived code maintained under ``components/`` and parts of ``modules/``;
- ESP Component Registry packages resolved from ``idf_component.yml``. The exact component set and versions can vary with the ESP-IDF version, selected chip, and board.

For the version, local path, and license of each dependency, see the :doc:`../license/index` inventory.

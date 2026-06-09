Project Relationships and Licensing
===================================

:link_to_translation:`zh_CN:[中文]`

This chapter explains how ESP-VISION relates to MicroPython, OpenMV, and its
major third-party components, and how to interpret their licenses when
distributing source code or firmware. It is developer guidance, not legal
advice; the license text and source-file headers for the exact version in use
remain authoritative.

ESP-VISION, MicroPython, and OpenMV
----------------------------------------

The relationship can be summarized as follows:

- **MicroPython provides the language runtime and firmware base.** ESP-VISION
  starts from a pinned MicroPython ESP32 port and adds
  ``overlay/micropython/``, MicroPython user C modules, and ESP-IDF components.
  It does not implement a separate Python interpreter.
- **OpenMV is an upstream source for part of the vision implementation and API
  design.** ESP-VISION reuses a subset of OpenMV ``imlib`` algorithms and some
  ``image`` binding and helper code, preserving their original license and
  copyright notices.
- **ESP-VISION is an independent integration for Espressif chips.** Camera,
  display, storage, USB, codec, ESP-DL inference, and board support are
  integrated using ESP-VISION and ESP-IDF components. ESP-VISION is not a fork
  of the OpenMV firmware and does not include the complete OpenMV hardware
  abstraction, IDE, or feature set.

The ``sensor`` and ``image`` modules therefore follow the OpenMV API style to
make script migration easier, but **matching API names do not imply complete
behavioral or feature compatibility**. Support also depends on the chip, board,
memory, peripherals, and build options. Use this guide's
:doc:`../api-reference/index` and :doc:`../examples/index` as the authoritative
support references.

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
      |      +-- display / imageio / espdl / h264 / rtsp
      |
      +-- Algorithms and middleware
      |      +-- OpenMV imlib subset
      |      +-- ulab / ZXing-C++ / ESP-DL
      |
      +-- ESP-IDF, managed components, and board backends

Dependencies come from three locations:

- Git submodules under ``lib/``, such as MicroPython, ``ulab``, and ZXing-C++;
- third-party-derived code maintained under ``components/`` and parts of
  ``modules/``;
- ESP Component Registry packages resolved from ``idf_component.yml``. The
  exact component set and versions can vary with the ESP-IDF version, target
  chip, and board.

Major Third-Party Code
----------------------

The following table lists direct dependencies most relevant to understanding
the project relationships. It is not a complete inventory of transitive
dependencies.

.. list-table::
   :header-rows: 1
   :widths: 18 23 35 24

   * - Project or code
     - Local location
     - Role in ESP-VISION
     - Primary license
   * - MicroPython v1.28.0
     - ``lib/micropython``
     - Python runtime and ESP32 port baseline
     - MIT
   * - micropython-ulab 6.12
     - ``lib/ulab``
     - Array and numerical computing for MicroPython
     - MIT
   * - OpenMV ``imlib`` subset
     - ``components/imlib``
     - Image representation, drawing, and vision algorithms
     - Primarily MIT, with per-file exceptions
   * - OpenMV Python binding code
     - ``modules/py_image*``, ``modules/py_helper*``, and related files
     - Exposes part of the OpenMV-style image API to MicroPython
     - MIT
   * - AprilTag implementation
     - ``components/imlib/upstream/apriltag.c``
     - AprilTag and related detection algorithms
     - BSD-2-Clause
   * - ZXing-C++ v3.0.2
     - ``lib/zxing-cpp``
     - Barcode reader backend
     - Apache-2.0
   * - ESP-DL
     - ESP Component Registry
     - Neural-network model inference
     - MIT
   * - ESP-IDF and other managed components
     - External SDK, ``managed_components/``
     - Drivers, networking, USB, video, codecs, and related services
     - Licensed per component or file

OpenMV and GPL Code Paths
-------------------------

Not every optional OpenMV upstream algorithm uses the same license. ESP-VISION
sets ``OMV_NO_GPL=1`` in ``micropython.cmake`` and
``components/imlib/CMakeLists.txt`` so GPL-conditioned OpenMV code paths are
not compiled. This switch only describes what the default build excludes; it
is not an automatic license classification for newly added files.

When changing ``imlib`` feature switches, synchronizing OpenMV upstream code,
or adding algorithms, inspect each file's license. Do not infer a license only
from the directory containing the file.

How to Interpret the Licenses
-----------------------------

The repository-level ``LICENSE`` is Apache License 2.0 and covers original
ESP-VISION code. It does not override or replace third-party licenses:

- third-party files remain under their original licenses and retain their
  original copyright and license headers;
- a directory can contain multiple licenses: most ``imlib`` code is MIT while
  ``apriltag.c`` is BSD-2-Clause;
- permissive licenses such as Apache-2.0, MIT, and BSD generally allow combined
  distribution, but their copyright, license text, and notice requirements
  still apply;
- some Espressif components use licenses with product-scope conditions. For
  example, the current ``esp_new_jpeg`` license text limits use to Espressif
  products, so it should not be described only as unrestricted generic MIT;
- trademarks, patents, model files, and datasets may have separate terms that
  cannot be inferred from the source-code license.

Before distributing source or firmware, check at least:

#. the repository-level ``LICENSE``;
#. the ``LICENSE`` in every Git submodule;
#. ``managed_components/*/LICENSE`` from the selected build;
#. SPDX identifiers, license headers, and copyright notices in modified or
   newly added source files;
#. provenance and terms for non-code assets such as models, fonts, and test
   images.

Adding Third-Party Code
-----------------------

When adding a third-party package or source file to ESP-VISION:

#. pin a reproducible release or commit;
#. verify the license per file and its compatibility with the intended
   distribution;
#. preserve upstream copyright, SPDX identifiers, license text, and any
   required NOTICE;
#. record the source, version, local path, purpose, and license in the
   documentation or repository license inventory;
#. verify that build switches actually exclude code that is not intended for
   distribution instead of relying on documentation alone;
#. obtain legal review before merging when licensing is unclear or imposes
   commercial-distribution constraints.

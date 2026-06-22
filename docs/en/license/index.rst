Licensing
=========

:link_to_translation:`zh_CN:[中文]`

This chapter is the single reference for how licensing works across ESP-VISION. It is developer guidance, not legal advice; the license text and source-file headers for the exact version in use remain authoritative.

ESP-VISION's own code (``platform/``, most of ``modules/``, ``boards/``, and build files) is released under Apache License 2.0, recorded in the repository-level ``LICENSE``. Vendored and third-party code keeps the license declared by its own source files, SPDX identifiers, or upstream license; the repository-level ``LICENSE`` does not override or replace those.

License Inventory
-----------------

The following table lists the direct dependencies most relevant to redistribution. It is not a complete inventory of transitive dependencies.

.. list-table::
   :header-rows: 1
   :widths: 22 26 32 20

   * - Project or code
     - Local path
     - Role in ESP-VISION
     - License
   * - `MicroPython <https://github.com/micropython/micropython>`__ v1.28.0
     - ``lib/micropython``
     - Python runtime and ESP32 port baseline
     - MIT
   * - `micropython-ulab <https://github.com/v923z/micropython-ulab>`__ 6.12
     - ``lib/ulab``
     - Array and numerical computing
     - MIT
   * - `OpenMV <https://github.com/openmv/openmv>`__ ``imlib`` subset
     - ``components/imlib``
     - Image processing and drawing algorithms
     - MIT, with separately licensed files
   * - OpenMV Python binding code
     - ``modules/py_image*``, ``modules/py_helper*``
     - OpenMV-style ``image`` API bindings
     - MIT
   * - OpenMV AprilTag implementation
     - ``components/imlib/upstream/apriltag.c``
     - AprilTag and rectangle detection
     - BSD-2-Clause
   * - `ZXing-C++ <https://github.com/zxing-cpp/zxing-cpp>`__ v3.0.2
     - ``lib/zxing-cpp``
     - 1D barcode reader backend
     - Apache-2.0
   * - `ESP-DL <https://github.com/espressif/esp-dl>`__
     - Component Registry
     - Model inference runtime
     - MIT
   * - `TensorFlow Lite Micro / esp-tflite-micro <https://github.com/espressif/esp-tflite-micro>`__
     - Component Registry
     - TensorFlow Lite Micro inference runtime
     - Apache-2.0
   * - `esp_new_jpeg <https://github.com/espressif/esp-adf-libs/tree/master/esp_new_jpeg>`__
     - Component Registry
     - Software JPEG codec
     - Espressif MIT (product-scope)
   * - `esp32-camera <https://github.com/espressif/esp32-camera>`__
     - Component Registry
     - Camera driver
     - Apache-2.0
   * - `ESP-IDF <https://github.com/espressif/esp-idf>`__
     - External SDK
     - Build system, drivers, and media components
     - Apache-2.0

How to Interpret the Licenses
-----------------------------

The repository-level ``LICENSE`` covers original ESP-VISION code and does not override third-party licenses:

- third-party files remain under their original licenses and retain their original copyright and license headers;
- a directory can contain multiple licenses: most ``imlib`` code is MIT while ``apriltag.c`` is BSD-2-Clause;
- permissive licenses such as Apache-2.0, MIT, and BSD generally allow combined distribution, but their copyright, license text, and notice requirements still apply;
- some Espressif components use licenses with product-scope conditions. For example, the current ``esp_new_jpeg`` license text limits use to Espressif products, so it should not be described only as unrestricted generic MIT;
- trademarks, patents, model files, and datasets may have separate terms that cannot be inferred from the source-code license.

Before distributing source or firmware, check at least:

#. the repository-level ``LICENSE``;
#. the ``LICENSE`` in every Git submodule;
#. ``managed_components/*/LICENSE`` from the selected build;
#. SPDX identifiers, license headers, and copyright notices in modified or newly added source files;
#. provenance and terms for non-code assets such as models, fonts, and test images.

OpenMV and GPL Code Paths
-------------------------

Not every optional OpenMV upstream algorithm uses the same license. ESP-VISION sets ``OMV_NO_GPL=1`` in ``micropython.cmake`` and ``components/imlib/CMakeLists.txt`` so GPL-conditioned OpenMV code paths are not compiled. This switch only describes what the default build excludes; it is not an automatic license classification for newly added files.

When changing ``imlib`` feature switches, synchronizing OpenMV upstream code, or adding algorithms, inspect each file's license. Do not infer a license only from the directory containing the file.

Adding Third-Party Code
-----------------------

When adding a third-party package or source file to ESP-VISION:

#. pin a reproducible release or commit;
#. verify the license per file and its compatibility with the intended distribution;
#. preserve upstream copyright, SPDX identifiers, license text, and any required NOTICE;
#. record the source, version, local path, purpose, and license in this inventory;
#. verify that build switches actually exclude code that is not intended for distribution instead of relying on documentation alone;
#. obtain legal review before merging when licensing is unclear or imposes commercial-distribution constraints.

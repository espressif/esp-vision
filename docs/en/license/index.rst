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
   * - `esptool-js 0.7.1 <https://github.com/espressif/esptool-js/tree/v0.7.1>`__
     - ``host/device-core/package.json`` dependency; loaded lazily by ``src/rom_loader.js``
     - Shared browser Web Serial / Node ROM backend; upstream ``src/targets/stub_flasher/README.md`` identifies esp-flasher-stub v1.2.2 (set 2)
     - Apache-2.0 (installed package ``LICENSE``); stub binaries dual Apache-2.0/MIT. Installed as a dependency; per-instance adapters do not modify upstream source. Transitive pako retains its MIT/Zlib notices; atob-lite retains MIT.
   * - @serialport/bindings-cpp 13.0.0; Koffi 2.16.3
     - ``host/device-core/package.json`` optional dependencies; CLI/plugin native bundles
     - Native serial access and Linux USJ modem controls; original npm prebuilds and loaders
     - MIT; original package licenses and Koffi Node API/cnoke notices retained in portable bundles
   * - Device Core host SDK
     - ``host/device-core``
     - EV-MUX v4 host communication, Python, resource files, discovery and images
     - Apache-2.0 (``host/device-core/LICENSE``); historical MIT notices in ``NOTICE``
   * - Device-side camera/LCD test fixture
     - ``host/device-core/tests/fixtures/device-checks.py``
     - Hardware acceptance functions migrated from the firmware tests; included in the npm package for shared hardware checks
     - Apache-2.0 (original notice retained)
   * - `sync-pr-to-gitlab <https://github.com/espressif/sync-pr-to-gitlab>`__ (6ea5db6)
     - ``.github/actions/sync-pr-to-gitlab``
     - GitHub PR to GitLab MR synchronization, adapted to remove Jira-specific metadata; original Python copyright and license notice retained
     - Apache-2.0
   * - `MicroPython <https://github.com/micropython/micropython>`__ v1.28.0
     - ``lib/micropython``
     - Python runtime and ESP32 port baseline
     - MIT
   * - MicroPython execution and thread adaptations
     - ``overlay/micropython/shared/runtime/pyexec.c``, ``overlay/micropython/ports/esp32/mpthreadport.c``
     - Session origin capture and inheritance based on the pinned upstream files; original copyright and license headers retained
     - MIT
   * - MicroPython composite USB descriptor adaptation
     - ``overlay/micropython/shared/tinyusb/mp_usbd_descriptor.c`` (v1.28.0 baseline)
     - Built-in CDC/MSC/UVC descriptors and speed variants; original upstream header preserved
     - MIT
   * - OV2710 timer lifecycle adaptation
     - ``tools/prepare_ov2710.py``, ``platform/ov2710_lifecycle.inc`` and verbatim ``tools/tests/fixtures/ov2710/upstream.c``; esp_cam_sensor 2.4.0, commit ``8fc931636ffa937c89cab53b53f4ce380190c1a4``
     - Checked build copy of ``sensors/ov2710/ov2710.c`` (SHA-256 ``4a9af395151527b156b1d685c39b298502490f33a8391a641a51ac72b3bc8876``); acknowledge callback shutdown before SCCB release, preserving the original header without editing the managed component
     - Apache-2.0
   * - TinyUSB UVC transfer adaptation
     - ``overlay/micropython/shared/tinyusb/esp_vision_video_transfer.inc``, ``tools/tests/tinyusb_video_stubs/upstream_transfer.c``; espressif/tinyusb 0.19.0~3, commit ``ab4a1817907ed865b10a712b5e03e5e0a9902df5``
     - Checked build copy of ``src/class/video/video_device.c``; terminate failed/short ISO frames, separate failure completion, and preserve the original copyright and license headers
     - MIT
   * - `micropython-ulab <https://github.com/v923z/micropython-ulab>`__ 6.12
     - ``lib/ulab``
     - Array and numerical computing
     - MIT
   * - `OpenMV v5.0.0 <https://github.com/openmv/openmv/tree/v5.0.0>`__ ``imlib`` subset
     - ``components/imlib``
     - Image processing and drawing algorithms
     - MIT, with separately licensed files
   * - OpenMV Python binding code
     - ``modules/py_image*``, ``modules/py_helper*``
     - OpenMV-style ``image`` API bindings
     - MIT
   * - OpenMV software JPEG adaptation
     - ``components/imlib/upstream/jpege.c``
     - Initialize the quantizer cache with an invalid sentinel so a first encode at quality 100 initializes its tables; original OpenMV MIT notice retained
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
   * - `NanoTrack V2 pretrained models <https://github.com/HonglinChu/SiamTrackers/tree/248663fde6bf7c40190cf10ee396d5662919ecd3/NanoTrack/models/nanotrackv2>`__
     - ``models/nanotrack_v2``
     - Locally quantized single-object tracking assets
     - Upstream NanoTrack supplies Apache-2.0 text; separate weight terms not verified (see local README and provenance)

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

Native binary-upload tests use the system OpenSSL 3 libcrypto (Apache-2.0) as their SHA-256 oracle through a PSA test shim. This is a host-test dependency only; OpenSSL and the shim are not linked into firmware or distributed in the npm package. Firmware uses ESP-IDF PSA Crypto.

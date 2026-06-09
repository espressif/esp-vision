Target and Board Support
========================

:link_to_translation:`zh_CN:[中文]`

ESP-VISION documentation is built separately for each IDF target. Select the
same target as the board firmware so module and API availability match the
default build.

.. list-table::
   :header-rows: 1
   :widths: 16 29 30 25

   * - Target
     - Current boards
     - Default modules
     - Additional constraints
   * - ESP32-P4
     - ``ESP32_P4X_EYE``, ``ESP32_P4X_FUNCTION_EV_BOARD``
     - ``sensor``, ``image``, ``display``, ``espdl``, ``image.ImageIO``,
       ``h264``, ``rtsp``
     - Current board profiles enable the ZXing-C++ barcode backend.
   * - ESP32-S3
     - ``ESP32_S3_EYE``
     - ``sensor``, ``image``, ``display``, ``espdl``, ``image.ImageIO``
     - ``h264``, ``rtsp``, and the barcode backend are not compiled by default.
   * - ESP32-S31
     - ``ESP32_S31_KORVO``
     - ``sensor``, ``image``, ``display``, ``espdl``, ``image.ImageIO``
     - Requires the ESP-IDF ``master`` overlay. ``h264``, ``rtsp``, and the
       barcode backend are not compiled by default.

Target Versus Board Capabilities
--------------------------------

The target controls chip-level source selection. For example, ``h264`` and
``rtsp`` are added by ``micropython.cmake`` only when ``IDF_TARGET`` is
``esp32p4``. A board profile can further enable or disable features such as the
ZXing-C++ barcode backend through ``boards/<BOARD>/board.cmake``.

The API navigation and target-specific symbol conditions are generated from
these build files. This makes ``micropython.cmake``,
``overlay/micropython/ports/esp32/boards/*/mpconfigboard.cmake``, and board
``board.cmake`` files authoritative for ESP-VISION API availability.

Standard MicroPython Features
-----------------------------

Standard MicroPython features use a separate configuration path:
``overlay/micropython/ports/esp32/mpconfigport.h`` plus each board's
``mpconfigboard.h``. Some conditions also depend on the ESP-IDF version or SoC
capability macros. For example, ESP-IDF 6.0 or newer disables the current port's
``network``, WLAN, SSL, ``cryptolib``, ``machine.I2S``, and ESP32 PCNT support.
The current board headers also disable Bluetooth and ESP-NOW; the S31 board
additionally disables ``machine.ADC`` and ``machine.ADCBlock``.

Because these conditions are not target-only, the target selector filters the
ESP-VISION API reference but does not claim to be a complete standard
MicroPython module manifest.

The target documentation describes the defaults shared by the current board
profiles. A custom board or modified build option can therefore differ from the
published target page. When that happens, the firmware configuration is
authoritative.

Building a Target
-----------------

.. code-block:: bash

   build-docs -l en -t esp32p4
   build-docs -l en -t esp32s3
   build-docs -l en -t esp32s31

The output is written to ``docs/_build/<language>/<target>/html/``.

Chip and Board Support
======================

:link_to_translation:`zh_CN:[中文]`

ESP-VISION documentation is built separately for each supported chip. Select the chip used by the board firmware so that the documented modules and APIs match the default build.

Supported Development Boards
----------------------------

.. include:: _generated/boards.rst

Chip Versus Board Capabilities
------------------------------

The selected chip controls chip-level source selection. For example, ``h264`` and ``rtsp`` are added by ``micropython.cmake`` only when ``IDF_TARGET`` is ``esp32p4``. A board profile can further enable or disable features such as the ZXing-C++ barcode backend through ``boards/<BOARD>/board.cmake``.

The API navigation and chip-specific symbol conditions are generated from these build files. This makes ``micropython.cmake``, ``boards/*/port/mpconfigboard.cmake``, and board ``board.cmake`` files authoritative for ESP-VISION API availability.

Standard MicroPython Features
-----------------------------

Standard MicroPython features use a separate configuration path: ``overlay/micropython/ports/esp32/mpconfigport.h`` plus each board's ``mpconfigboard.h``. The table above indexes the explicitly enabled baseline features from those files, while additional conditions can depend on chip capabilities and firmware configuration.

The current board headers disable Bluetooth and ESP-NOW; the S31 board additionally disables ``machine.ADC`` and ``machine.ADCBlock``. A board must still provide suitable networking hardware and firmware configuration before ``network.WLAN`` can connect.

Because these conditions are not determined solely by the chip, the chip selector filters the ESP-VISION API reference but does not represent a complete standard MicroPython module manifest. Build-environment compatibility details are maintained in the project README.

The chip documentation describes the defaults shared by the current board profiles. A custom board or modified build option can therefore differ from the published chip page. When that happens, the firmware configuration is authoritative.

WebREPL and EV-MUX Network Control
----------------------------------

Traditional MicroPython WebREPL is not supported by the current ESP-VISION EV-MUX runtime, including ESP-IDF v5.5.6 builds. Some builds enable WebSocket and the internal ``_webrepl`` component. The upstream ``webrepl`` Python service is absent from some board profiles, including ESP32_P4X_EYE; ESP32_S3_EYE includes it in its manifest. Neither its presence nor the compile flags establish terminal or file-transfer compatibility with EV-MUX.

EV-MUX routes Python input, output and files through one authorized owner, with link, lease and VM-epoch checks. A traditional WebREPL ``dupterm`` service does not implement this ownership contract. Adding its Python package or mirroring raw terminal bytes is not a supported workaround. Use :doc:`../device-core/api` with a TCP transport for network control. The retained ``example/07-Network/00-WebREPL/webrepl.py`` entry exits with this explanation before changing Wi-Fi or opening a listener.

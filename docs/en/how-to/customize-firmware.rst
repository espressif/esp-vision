Customize Firmware Features
===========================

:link_to_translation:`zh_CN:[中文]`

ESP-VISION separates application-facing modules, image algorithms, standard MicroPython features, frozen Python code, and board services into independent configuration layers. This lets a product firmware remove unused capabilities to reduce flash, RAM, dependencies, and attack surface, or add product-specific modules without rewriting the camera and media pipeline.

Choose the Customization Scope
------------------------------

Before editing configuration, decide whether the change belongs to every ESP-VISION firmware or only one board. Changes to the root ``micropython.cmake`` affect every board whose chip matches the edited condition. Changes under ``boards/<BOARD>/`` (including the MicroPython-port files in ``boards/<BOARD>/port/``) are board-specific. For a product variant, create a dedicated board package as described in :doc:`add-board` instead of changing a shared development-board profile.

Customize ESP-VISION Python Modules
-----------------------------------

The ``ESP_VISION_MODULE_SOURCES`` list in ``micropython.cmake`` defines the C/C++ bindings exposed to Python. Remove a source only together with any helper source it requires, or append a new source as described in :doc:`add-python-module`. Chip-specific modules belong inside an ``IDF_TARGET`` condition; for example, H.264 and RTSP are currently enabled only for ``esp32p4``.

.. code-block:: cmake

   set(ESP_VISION_MODULE_SOURCES
       ${ESP_VISION_ROOT}/modules/py_display.c
       ${ESP_VISION_ROOT}/modules/py_image.c
       ${ESP_VISION_ROOT}/modules/py_imageio.c
       ${ESP_VISION_ROOT}/modules/py_helper.c
       ${ESP_VISION_ROOT}/modules/py_sensor.c
   )

Removing a binding does not automatically remove every platform service or managed component behind it. After changing the source list, inspect ``target_sources``, ``target_link_libraries``, component manifests, and the final size report to confirm that the unwanted dependency is no longer linked.

Customize Image Algorithms
--------------------------

Each board's ``boards/<BOARD>/imlib_config.h`` selects optional ``imlib`` algorithms. Remove an ``IMLIB_ENABLE_*`` definition to omit an unused algorithm, or add a supported definition to expose another algorithm. Common groups include filters, geometry detection, QR codes, and AprilTags.

.. code-block:: c

   #define IMLIB_ENABLE_MEAN
   #define IMLIB_ENABLE_GAUSSIAN
   #define IMLIB_ENABLE_QRCODES
   #define IMLIB_ENABLE_APRILTAGS

Some Python methods remain present in the binding even when their backend is disabled and will raise an exception at runtime. Keep the API documentation, examples, and generated board support table aligned with the selected algorithms. Do not remove ``OMV_NO_GPL`` without completing a license review.

Customize Standard MicroPython Features
---------------------------------------

Use ``boards/<BOARD>/port/mpconfigboard.h`` to override standard MicroPython feature macros for one board, such as networking, Bluetooth, ESP-NOW, ADC, SD card, USB, or other ``MICROPY_PY_*`` and ``MICROPY_HW_*`` options. Use ``mpconfigboard.cmake`` for CMake-level options and the board's sdkconfig chain.

.. code-block:: c

   #define MICROPY_PY_BLUETOOTH (0)
   #define MICROPY_PY_ESPNOW (0)
   #define MICROPY_PY_NETWORK_WLAN (1)

These macros can depend on ESP-IDF versions and SoC capability macros. A disabled Python feature may also require removing its ESP-IDF configuration or managed dependency before it produces a measurable firmware-size reduction.

Configure Memory Placement
--------------------------

PSRAM boards use the board's ``sdkconfig`` chain under ``boards/<BOARD>/port/`` to set the allocation boundary. ``CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`` is the byte threshold used by ESP-IDF's default allocator and ESP-VISION's automatic allocator policy: allocations up to the threshold prefer internal RAM, while larger allocations prefer PSRAM. ``CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`` separately reserves internal RAM for drivers and other allocations that require it. Adjust these values in the board's sdkconfig defaults when changing the memory balance.

Large image, model, frame, and scratch buffers prefer PSRAM when their required capabilities allow it. When a caller requires direct DMA or executable access, allocator fallbacks retain that capability. The P4X_EYE SPI display can also use an ordinary 8-bit framebuffer as a draw source because the LCD transfer layer stages it into bounded DMA transfers; this explicit copy path does not weaken direct-DMA requirements. Boards without PSRAM use internal memory where the capability is available.

Customize Frozen Python Code
----------------------------

The board's ``boards/<BOARD>/manifest.py`` controls Python modules frozen into firmware. Add product startup code, libraries, or packages with ``freeze()``, ``module()``, ``package()``, or ``include()`` as supported by the MicroPython manifest system. Remove an entry when that frozen module is not required.

.. code-block:: python

   freeze("$(PORT_DIR)/modules")
   freeze("$(ESP_VISION_ROOT)/modules", "py_inisetup.py")
   freeze("$(ESP_VISION_ROOT)/boards/<BOARD>", "board_inisetup.py")
   include("$(MPY_DIR)/extmod/asyncio")

Freezing code improves deployment consistency and startup availability but consumes firmware flash. Files intended to remain replaceable during development or after deployment should stay on the root filesystem, such as packages under ``/lib``, or on ``/sdcard`` instead.

Customize Board Services and Optional Components
------------------------------------------------

Board-specific camera, display, and SD card implementations live under ``boards/<BOARD>/``. ``micropython.cmake`` automatically selects ``camera.c``, ``display.c``, and ``sdcard.c`` when present. Use ``boards/<BOARD>/board.cmake`` for board-level CMake switches; for example, current P4 boards enable the optional ZXing barcode backend with ``ESP_VISION_ENABLE_BARCODE``.

.. code-block:: cmake

   set(ESP_VISION_ENABLE_BARCODE OFF)

When adding a component, register its source or IDF component and link it to ``usermod_esp_vision_platform``. When removing one, remove the source, include path, compile definition, link dependency, and component-manifest entry as a single change.

Build and Verify
----------------

Reconfigure after changing CMake, sdkconfig, manifests, or feature macros, then build the selected board:

.. code-block:: bash

   idf.py --board <BOARD> reconfigure
   idf.py --board <BOARD> build
   idf.py --board <BOARD> size

Verify imports and affected APIs on the REPL, run the relevant examples, and compare the size report before and after the change. Also rebuild the documentation because module navigation and board capability summaries are generated from the firmware configuration.

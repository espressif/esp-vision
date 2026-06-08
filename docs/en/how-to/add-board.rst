Add a New Board
===============

:link_to_translation:`zh_CN:[中文]`

A board package is split across two locations: the MicroPython ESP32 port overlay
(``overlay/micropython/ports/esp32/boards/<BOARD>/``) and the ESP-VISION repo
(``boards/<BOARD>/``). Start from the ``TEMPLATE`` board.

MicroPython Port Side
---------------------

In ``overlay/micropython/ports/esp32/boards/<BOARD>/``:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - File
     - Purpose
   * - ``mpconfigboard.cmake``
     - IDF target and ``SDKCONFIG_DEFAULTS`` chain.
   * - ``mpconfigboard.h``
     - MicroPython feature flags and USB strings.
   * - ``sdkconfig.board``
     - Board-specific ESP-IDF Kconfig overrides.
   * - ``partitions-*.csv``
     - Partition table.
   * - ``board.json``, ``board.md``
     - Upstream board manifest metadata.

ESP-VISION Side
---------------

In ``boards/<BOARD>/``:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - File
     - Purpose
   * - ``boardconfig.h``
     - Pin assignments and board runtime constants.
   * - ``imlib_config.h``
     - OpenMV ``imlib`` feature switches.
   * - ``manifest.py``
     - Frozen Python modules.
   * - ``camera.c``
     - Board-specific camera backend.
   * - ``display.c``
     - LCD panel and backlight implementation.
   * - ``sdcard.c``
     - SD card power and card-detect implementation.

``micropython.cmake`` automatically picks up ``camera.c``, ``display.c``, and
``sdcard.c`` from the board directory when present, and includes the board's
optional ``board.cmake``.

Build and Flash
---------------

.. code-block:: bash

   make BOARD=<NEW_BOARD> ESPPORT=/dev/ttyACM0 build flash monitor
   idf.py --board <NEW_BOARD> -p /dev/ttyACM0 build flash monitor

.. note::

   This page is a starting outline. Detailed bring-up steps (sensor selection,
   PPA configuration, display timing) will be expanded.

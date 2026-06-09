Get Started
===========

:link_to_translation:`zh_CN:[中文]`

Prerequisites
-------------

- ESP-IDF ``release/v5.5``, ``release/v6.0``, or ``master`` with the export
  script sourced so that ``idf.py`` is on ``PATH``.
- A board listed in :doc:`../target-support/index` for the selected target.

.. only:: esp32s31

   ESP32-S31 currently requires an ESP-IDF ``master`` environment.

Build, Flash, and Monitor
-------------------------

Clone the repository with submodules, then build with ``make``:

.. code-block:: bash

   git clone --recursive <this-repo> esp-vision
   cd esp-vision

.. only:: esp32p4

   .. code-block:: bash

      make BOARD=ESP32_P4X_EYE ESPPORT=/dev/ttyACM0 build flash monitor

.. only:: esp32s3

   .. code-block:: bash

      make BOARD=ESP32_S3_EYE ESPPORT=/dev/ttyACM0 build flash monitor

.. only:: esp32s31

   .. code-block:: bash

      make BOARD=ESP32_S31_KORVO ESPPORT=/dev/ttyACM0 build flash monitor

Or use the board-aware ``idf.py`` extension from the repository root:

.. only:: esp32p4

   .. code-block:: bash

      idf.py --board ESP32_P4X_EYE -p /dev/ttyACM0 build flash monitor

.. only:: esp32s3

   .. code-block:: bash

      idf.py --board ESP32_S3_EYE -p /dev/ttyACM0 build flash monitor

.. only:: esp32s31

   .. code-block:: bash

      idf.py --board ESP32_S31_KORVO -p /dev/ttyACM0 build flash monitor

Both entry points first run ``prepare-micropython``: they verify that
``lib/micropython`` is checked out at the pinned MicroPython v1.28.0 commit,
export a clean MicroPython build copy under ``build/micropython/``, then apply
``overlay/micropython/`` to that copy. ``lib/micropython`` is never dirtied.

Common Make Targets
-------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Command
     - Description
   * - ``make BOARD=<BOARD> build``
     - Build firmware for a board.
   * - ``make BOARD=<BOARD> ESPPORT=<PORT> deploy``
     - Build and flash.
   * - ``make BOARD=<BOARD> ESPPORT=<PORT> monitor``
     - Open the serial monitor.
   * - ``make BOARD=<BOARD> menuconfig``
     - Open menuconfig.
   * - ``make BOARD=<BOARD> ESPPORT=<PORT> erase``
     - Erase flash.
   * - ``make BOARD=<BOARD> clean``
     - Clean board build output.
   * - ``make distclean``
     - Wipe all build output.

Run Your First Script
---------------------

After flashing, connect over the REPL and try the camera. See
:doc:`../examples/index` for ready-to-run scripts.

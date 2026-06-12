Get Started
===========

:link_to_translation:`zh_CN:[中文]`

Prerequisites
-------------

- A supported ESP-IDF environment with its export script sourced so that ``idf.py`` is on ``PATH``; see the project README for the maintained branch compatibility.
- A board listed in :doc:`../target-support/index` for the selected chip.

Build, Flash, and Monitor
-------------------------

Clone the repository with submodules, then use the board-aware ``idf.py`` extension from the repository root:

.. code-block:: bash

   git clone --recursive https://github.com/espressif/esp-vision.git esp-vision
   cd esp-vision

.. only:: esp32p4

   .. code-block:: bash

      idf.py --board ESP32_P4X_EYE -p /dev/ttyACM0 build flash monitor

.. only:: esp32s3

   .. code-block:: bash

      idf.py --board ESP32_S3_EYE -p /dev/ttyACM0 build flash monitor

.. only:: esp32s31

   .. code-block:: bash

      idf.py --board ESP32_S31_KORVO -p /dev/ttyACM0 build flash monitor

The command first runs ``prepare-micropython``: it verifies that ``lib/micropython`` is checked out at the pinned MicroPython v1.28.0 commit, exports a clean MicroPython build copy under ``build/micropython/``, applies ``overlay/micropython/`` to that copy, then projects each ``boards/<BOARD>/port/`` onto the copy's ``ports/esp32/boards/<BOARD>/``. ``lib/micropython`` is never dirtied.

Common idf.py Commands
----------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Command
     - Description
   * - ``idf.py --board <BOARD> build``
     - Build firmware for a board.
   * - ``idf.py --board <BOARD> -p <PORT> flash``
     - Build and flash firmware.
   * - ``idf.py --board <BOARD> -p <PORT> monitor``
     - Open the serial monitor.
   * - ``idf.py --board <BOARD> menuconfig``
     - Open menuconfig.
   * - ``idf.py --board <BOARD> -p <PORT> erase-flash``
     - Erase flash.
   * - ``idf.py --board <BOARD> clean``
     - Clean board build output.
   * - ``idf.py --board <BOARD> fullclean``
     - Remove the complete build directory for the selected board.

Run Your First Script
---------------------

After flashing, connect over the REPL and try the camera. Each :doc:`../api-reference/index` module page links the runnable ``example/`` scripts for that API.

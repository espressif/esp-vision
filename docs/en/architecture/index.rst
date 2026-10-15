Solution Architecture
=====================

:link_to_translation:`zh_CN:[中文]`

ESP-VISION separates host applications from device firmware. IDE, Agent, CLI and custom tools share Device Core; the SDK implements the host side of EV-MUX v4 over board-supported USB CDC, USB Serial/JTAG, UART or TCP. Device firmware combines MicroPython, vision/inference modules, platform services and board backends. Firmware code is further layered by whether it touches MicroPython (``mp_obj_t`` / ``py/*.h``).

.. figure:: ../../_static/esp-vision-architecture-adaptive.svg
   :align: center
   :alt: ESP-VISION layered architecture

   Host applications → Device Core → EV-MUX → device firmware

Layered Overview
----------------

- **Host applications**: IDE presents editing/debugging UI, Agent runs automation workflows, CLI handles terminal commands, and custom tools supply their own application policy. These are peer consumers of the same SDK.
- **Device Core** (``host/device-core``): the independently versioned ``@esp-vision/device-core`` npm package owns protocol parsing, request correlation, sessions, keepalive and operation lifecycles. The diagram selects representative API names instead of listing every method; complete contracts are in :doc:`../device-core/api`.
- **EV-MUX v4**: the runtime wire protocol between Core and firmware, covering framing, logical channels, ownership and VM epochs across USB CDC, USJ, UART and TCP. Host adapters and network bridges carry bytes without creating another session owner.
- **Device application**: MicroPython scripts run on the board and call its Python bindings. They can continue running after a host disconnects.
- **Bindings** (``modules/``): the ``USER_C_MODULES`` layer. The main modules ``image``, ``sensor``, ``display``, ``espdl``, and ``tflite`` self-register via ``MP_REGISTER_MODULE``. ``py_imageio.c`` provides the ``image.ImageIO`` type, and ``py_helper.c`` is shared helper code. Bindings only do object conversion and light API adaptation; heavy logic lives in pure C or ``platform/``.
- **Platform services** (``platform/``): self-written ESP32 glue. ``ev_channel.c`` / ``ev_mux.c`` / ``ev_control_transport.c`` / ``ev_stdio.c`` (EV-MUX / EV-ATP transport), ``preview.c`` (EV-MUX JPEG preview), ``display.c`` (generic display layer), ``sdcard.c`` (mount at ``/sdcard``), ``usb_msc.c`` (exposes the ``ffat`` partition over TinyUSB MSC), ``usb_auto_download.c`` (S3/P4 OTG CDC reset/download protocol), ``jpeg.c`` (hardware or software JPEG), ``debug.c``, and ``main.c`` (startup init plus the soft-reset loop).
- **imlib component** (``components/imlib/``): pure-C vision algorithms, an IDF component maintained as MIT, derived from OpenMV ``lib/imlib``.
- **Board backends** (``boards/<BOARD>/``): per-board configuration and the real camera/display/sdcard implementations. P4X and S31 use ``esp_video``/V4L2; P4X also uses PPA, while S3 uses ``esp32-camera``.
- **MicroPython + overlay**: MicroPython v1.28.0 is the fixed baseline; project changes live in ``overlay/micropython/`` and are applied to a generated build copy under ``build/micropython/``.

Host-provided byte transports implement physical I/O beneath the SDK protocol. LAN DNS-SD discovery is an enumeration path before connection; ROM firmware flashing uses a separate loader lifecycle and does not send application images through EV-MUX. Firmware and SDK versions remain independent. See :doc:`../device-core/index` for host integration and API documentation.

Capture-to-Output Data Flow
---------------------------

.. blockdiag::

   blockdiag {
     orientation = portrait;

     sensor [label = "Camera sensor"];
     snap   [label = "sensor.snapshot()"];
     img    [label = "image.Image\n(reusable framebuffer)"];
     proc   [label = "imlib processing /\nAI inference"];
     lcd    [label = "display.write()\n-> LCD"];
     prev   [label = "img.flush()\n-> EV-MUX preview"];

     sensor -> snap -> img;
     img -> proc;
     img -> lcd;
     img -> prev;
   }

``sensor.snapshot()`` captures a frame into a reusable framebuffer wrapped as an ``image.Image``. Scripts then run ``imlib`` processing, ESP-DL inference, or TFLite Micro inference on the image, and send it to the LCD with ``display.write()`` or to the host preview with ``img.flush()``.

Communication Subsystem
-----------------------

EV-MUX v4 owns user/debug streams together and multiplexes channels onto the current USJ, USB, UART or TCP owner link. MicroPython HAL, C stdio, IDF logs and JPEG preview have distinct producer paths.

Detailed architecture, interfaces, acquisition, framing, Python/files/images and acceptance are in :doc:`../communication/index`. The current Python traceback/sys.stderr path is documented in :doc:`../communication/architecture`. This keeps one canonical communication chapter rather than duplicating it in architecture and Agent guides.

Source Tree
-----------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Path
     - Responsibility
   * - ``idf_ext.py``
     - Board-aware ``idf.py`` extension for the repository root.
   * - ``micropython.cmake``
     - Integration hub: registers user modules, platform and board sources, include paths, conditional ``zxing``, and ``ulab``.
   * - ``lib/``
     - Pinned third-party submodules (MicroPython, ``ulab``, ZXing-C++).
   * - ``overlay/micropython/``
     - ESP-VISION MicroPython delta, using the MicroPython path layout.
   * - ``boards/``
     - Per-board config, frozen manifests, and board peripheral backends.
   * - ``platform/``
     - Shared runtime services (camera, preview, storage, display, USB, JPEG).
   * - ``modules/``
     - MicroPython C/C++ bindings (``sensor``, ``image``, ``display``, ``imageio``, ``espdl``, ``tflite``, plus chip-dependent ``h264`` and ``rtsp``).
   * - ``components/``
     - ESP-IDF components, including OpenMV ``imlib`` and the ZXing backend.
   * - ``models/``
     - Optional model assets loaded from board storage at runtime.
   * - ``example/``
     - MicroPython example scripts.
   * - ``stubs/``
     - ``.pyi`` type stubs describing the C modules.

Board Composition
-----------------

A board is defined in a single tree, ``boards/<BOARD>/``:

- ESP-VISION side (top level): ``boardconfig.h``, ``imlib_config.h``, ``manifest.py``, and optional ``camera.c`` / ``display.c`` / ``sdcard.c``.
- MicroPython port side (``boards/<BOARD>/port/``): ``IDF_TARGET`` value, sdkconfig, partitions, and USB strings. The build projects this subdirectory onto ``ports/esp32/boards/<BOARD>/`` of the generated MicroPython copy.

See :doc:`../how-to/add-board` for the step-by-step procedure.

Chip-Dependent Sources
----------------------

``micropython.cmake`` selects modules from ``IDF_TARGET`` and the board profile. The ESP32-P4 build includes ``h264`` and ``rtsp``; the current P4 board profiles also enable the ZXing-C++ barcode backend. See :doc:`../target-support/index` for the resulting public API matrix.

MicroPython Overlay
-------------------

ESP-VISION uses MicroPython v1.28.0 as a fixed upstream baseline. Project changes to the ESP32 port live under ``overlay/micropython/``. The ``prepare-micropython`` build step applies that tree to a generated copy under ``build/micropython/idf<ESP_IDF_VERSION>/micropython/``; the ``lib/micropython`` submodule remains a clean upstream reference.

For how ESP-VISION relates to its upstream projects, see :doc:`../project-relationship/index`; for the license of each component, see :doc:`../license/index`.

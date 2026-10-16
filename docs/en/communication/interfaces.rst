Physical interfaces, connection generations and build configuration
===================================================================

:link_to_translation:`zh_CN:[中文]`

Sink types and actual links
---------------------------

``capabilities.sinks`` describes compiled backends and their current state. Sink IDs are ``usj``, ``usb``, ``uart`` and ``tcp``, not OS paths such as ``/dev/ttyACM0`` or channels. ``usb_otg_cdc`` is a hardware kind; its endpoint ID remains ``usb``. Neither ``cdc`` nor ``usb_otg_cdc`` is a substitute.

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     host [label = "IDE / Agent / CLI / custom tools"];
     core [label = "Device Core"];
     protocol [label = "EV-MUX v4"];
     usj [label = "usj\nUSB Serial/JTAG"];
     usb [label = "usb\nTinyUSB CDC"];
     uart [label = "uart\nserial pins + adapter"];
     tcp [label = "tcp\nSTA IPv4 :3232"];
     owner [label = "One acquired link\nowns user + debug"];
     host -> core -> protocol;
     protocol -> usj; protocol -> usb; protocol -> uart; protocol -> tcp;
     usj -> owner; usb -> owner; uart -> owner; tcp -> owner;
   }

**Figure 6: Multiple physical ingress interfaces can discover/acquire the device, which has one control owner.** These interfaces do not represent separate VMs.

The host/Core/protocol relationship follows :doc:`../architecture/index`. This figure expands only the physical runtime links beneath EV-MUX; discovery and the ROM loader have separate lifecycles. Serial/socket adapters do not acquire another session.

.. list-table:: Physical interface comparison
   :header-rows: 1
   :widths: 13 29 29 29

   * - sink
     - Physical connection
     - Presence/readiness detection
     - Typical uses and limitations
   * - ``usj``
     - Chip USB-Serial/JTAG peripheral, usually exposed as a host serial port.
     - Without reliable unplug detection, ``present`` is unknown; silent disappearance relies on lease recovery.
     - P4-EYE default/fallback port; early ROM/IDF output may also appear. It uses a different driver from USB-OTG CDC.
   * - ``usb``
     - USB-OTG uses existing TinyUSB CDC and may share a composite device with MSC.
     - USB configuration establishes present/ready; actual disconnection invalidates link generation.
     - Suitable for interaction, files and images; application traffic does not require DTR. CDC and MSC have separate functions/write permissions.
   * - ``uart``
     - Physical UART TX/RX/GND and a USB-UART adapter.
     - Depends on board UART REPL enablement and initialization; peer disappearance is usually not reliably detectable.
     - Carries the same channels, subject to baud rate, pin/port allocation and bounded transmission deadlines.
   * - ``tcp``
     - STA IPv4 TCP server on a configured device.
     - Accept allocates a slot; FIN/RST, socket errors and timeouts reclaim links.
     - Default port 3232; two accepted slots do not mean two owners. Browsers require a WebSocket-to-TCP byte bridge.

Board backend enablement
------------------------

Backends depend on ``boards/<BOARD>/boardconfig.h``, ``boards/<BOARD>/port/mpconfigboard.h`` and SoC capabilities. Disabling a MicroPython interface macro disables that interface, rather than only excluding it from EV-MUX; there is no mux-specific serial allowlist. The table describes current source defaults; runtime ``capabilities.sinks`` is authoritative.

.. list-table:: Build conditions and current board examples
   :header-rows: 1
   :widths: 15 35 50

   * - sink
     - Build condition
     - Current board examples and runtime limitations
   * - ``usj``
     - SoC provides USJ and ``MICROPY_HW_ESP_USB_SERIAL_JTAG`` is enabled.
     - P4 boards and ``ESP32_S31_CHATBOT``; some raw IDF system-console output remains independently configured.
   * - ``usb``
     - SoC/port provides USB device support and ``MICROPY_HW_USB_CDC`` is effective.
     - ``ESP32_S3_EYE``, ``ESP32_S31_KORVO``, ``ESP32_S31_MOSAICO``, ``AtomS3R-M12``; P4 boards may expose both CDC and USJ according to SoC support.
   * - ``uart``
     - ``MICROPY_HW_ENABLE_UART_REPL`` enabled, using board pins/baud rate.
     - Enabled on P4, ``ESP32_S3_EYE``, ``AtomS3R-M12``, ``ESP32_S31_KORVO``; currently disabled on ``ESP32_S31_MOSAICO`` and ``ESP32_S31_CHATBOT``.
   * - ``tcp``
     - ``ESP_VISION_ENABLE_TCP_TRANSPORT=1`` with an available WLAN driver.
     - Listener requires STA IPv4. Current ``ESP32_P4X_VISION`` port configuration disables WLAN; compiling TCP does not prove runtime network availability.

Examples are ``boards/ESP32_P4X_EYE/boardconfig.h``, ``boards/ESP32_S3_EYE/boardconfig.h`` and ``boards/ESP32_S31_MOSAICO/port/mpconfigboard.h``; ``platform/ev_transport_config.h`` checks backend constraints.

Distinguishing USB-Serial/JTAG from USB-OTG
-------------------------------------------

P4-EYE can expose USJ and OTG CDC simultaneously. Both may appear as ``ttyACM``, whose OS numbering is unstable. Identify interfaces through USB descriptors, VID/PID, serial number and board topology, then confirm device identity through ``hello``/``capabilities``.

USJ and TinyUSB CDC use different peripherals/drivers. A hello response returns only to its actual ingress link, without duplication to the default interface. If one interface owns the session, the other can discover the device but acquisition returns busy.

The current P4-EYE runtime CDC PID is ``0x4003``. Some S3 builds use ``0x1001`` to select the esptool USB reset sequence; that Serial/JTAG-like PID does not give TinyUSB OTG CDC JTAG hardware capabilities. ROM mode may enumerate a different node, invalidating the original handle.

Link = backend + slot + generation
----------------------------------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     first [label = "TCP slot 0\ngeneration G"];
     close [label = "FIN / RST / close"];
     reuse [label = "TCP slot 0 reused\ngeneration G+1"];
     stale [label = "old queued request / frame\nslot 0, generation G"];
     check [label = "generation check\nreject stale work"];
     first -> close -> reuse;
     stale -> check; reuse -> check;
   }

**Figure 7: Reusing a TCP slot does not preserve the previous connection.** Sessions bind exact link references. TX targets, queued VM requests and asynchronous callbacks must not deliver old work to a new connection. Actual USB disconnection and explicit release establish corresponding invalidation boundaries.

Board configuration differs from IDF console configuration
----------------------------------------------------------

.. list-table:: Configuration layers
   :header-rows: 1
   :widths: 34 66

   * - Configuration
     - Purpose
   * - ``CONFIG_ESP_CONSOLE_*``
     - Underlying ESP-IDF system console, startup and exception output routing; does not directly determine EV-MUX ownership.
   * - ``MICROPY_HW_ESP_USB_SERIAL_JTAG``
     - MicroPython/ESP-VISION USJ compilation and initialization.
   * - ``MICROPY_HW_USB_CDC`` / MSC port macros
     - Existing TinyUSB CDC/MSC composition; no second USB driver or descriptor owner is installed.
   * - ``MICROPY_HW_ENABLE_UART_REPL``
     - Whether UART REPL becomes a registered serial backend.
   * - ``ESP_VISION_TRANSPORT_DEFAULT_SINK`` / ``ESP_VISION_TRANSPORT_FALLBACK_SINK``
     - Default/fallback serial output without an owner. Must be enabled ``usj``, ``usb`` or ``uart``; cannot default to unconfigured TCP.
   * - ``ESP_VISION_ENABLE_BACKGROUND_NETWORK``
     - Background Wi-Fi/TCP service policy; WLAN hardware, native/Hosted driver and sdkconfig requirements are separate.
   * - ``ESP_VISION_WIFI_DEFAULT_SSID`` / ``ESP_VISION_WIFI_DEFAULT_PASSWORD``
     - Optional factory Wi-Fi credentials, empty by default; used only when no NVS record exists.
   * - ``ESP_VISION_ENABLE_TCP_TRANSPORT`` / ``ESP_VISION_TCP_PORT``
     - Board TCP backend switch and listening port, normally 3232.

The backend registry is compile-time defined, with no runtime plugin-registration API. USB connection, network service and TCP slot state remain dynamic. ``supported``, ``present``, ``ready`` and ``owner`` mean implementation availability, known physical presence, traffic readiness and session ownership respectively; they are not interchangeable.

``console`` / ``null`` (alias ``none``) are local output options in ``ev_channel.c``, not public registered sinks in ``capabilities.sinks``. Do not use them in v4 acquisition requests.

Why UART cannot assume USB preview sizes
----------------------------------------

UART estimates its transmission budget for the entire frame:

.. code-block:: text

   timeout_ms = max(1500, ceil(frameBytes * 10 * 1000 / baud) * 2 + 250)
   timeout_ms <= 10000

The factor 10 represents the bits per byte for typical 8N1. Frames exceeding a 10-second budget are rejected before submission. Supporting ``preview.frame`` does not guarantee arbitrary JPEG delivery at low baud rates. Increase baud rate, reduce image resolution/quality or use USB/TCP; still handle TX errors and short writes.

Three distinct TCP time budgets
-------------------------------

1. **Initial handshake**: a connection that has never acquired a session closes after approximately 3000 ms. TCP clients should not perform the serial 300 ms startup drain or lengthy UI work first.
2. **Session lease**: 15000 ms by default; absence of authorized activity releases the owner. This reclaims ownership.
3. **Idle reclamation after ownership**: a socket that lost ownership has approximately 30000 ms to reacquire before its exact-generation slot is reclaimed. This reclaims connection capacity.

Two slots allow brief coexistence of discovery/competing connections, not two independent applications controlling the VM. A TCP contender closes after its acquisition error is sent. The same link can reacquire within the allowed window after normal release.

DTR/RTS, RESET and ROM download
-------------------------------

DTR changes do not acquire or release EV-MUX sessions. Closing a host serial handle while USB remains connected may be invisible to firmware. Send ``session.release`` before closing, or wait for lease expiry.

CDC DTR/RTS automatic download is a separate USB control path. It shares ``esp_vision_restart(download_mode)`` with Python ``machine.bootloader()``. Python can request an immediate restart; USB control callbacks record state and a deferred timer restarts outside the callback. P4 preserves other system-register bits when setting/clearing download bits; S3 follows its ROM USB persistence/PHY handoff contract.

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     Host -> Firmware [label = "EV-MUX: machine.bootloader()\nor USB DTR/RTS control"];
     Firmware -> Restart [label = "esp_vision_restart(true)"];
     Restart -> ROM [label = "masked boot flag + esp_restart"];
     Host -> ROM [label = "reopen enumerated ROM interface"];
     Host -> ROM [label = "ROM/stub flash protocol + readback"];
     ROM -> Firmware [label = "clear download latch + reset"];
     Host -> Firmware [label = "new hello/acquire + identity check"];
   }

**Figure 8: Entering download mode, writing images and restoring runtime require three separate confirmations.** EV-MUX does not carry ROM flashing commands. Loss of its port alone proves neither ROM readiness nor successful flashing.

For P4-EYE OTG runtime-to-ROM transitions, esptool v5 uses ``--before usb-reset``. Once in ROM, or retrying after re-enumeration, use ``--before no-reset``. v4 spellings are ``usb_reset`` / ``no_reset``. Verify writes, full readback, cleared download bits and runtime identity; an ``EIO`` alone is not success.

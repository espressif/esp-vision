Device management CLI
========================================

:link_to_translation:`zh_CN:[中文]`

The npm package installs ``esp-vision-device`` for management. Its four commands are inspect, configure, control and firmware (check/update). It calls the same public createDevice API as the IDE. There are no separate discover/info/status/capabilities commands, Python shell or file-manager commands.

Installation and platforms
----------------------------------------

.. code-block:: console

   npm install -g @esp-vision/device-core@0.1.0
   esp-vision-device --help
   esp-vision-device inspect

Installation requires the first npm publication. Use Node.js 22. Windows, Linux and macOS use the same command parser and system-port enumeration through Python/pyserial. Windows defaults to python; other systems default to python3. Use --python to select an interpreter with pyserial installed. No Linux-specific device scanning is used. LAN discovery uses optional multicast-dns, installed with npm unless optional dependencies are omitted; missing discovery dependencies produce a warning while successful sources remain usable.

.. code-block:: console

   python -m pip install pyserial
   esp-vision-device inspect --python python

Enumerating and querying
----------------------------------------

Without a target, inspect enumerates OS serial ports and DNS-SD announcements without opening serial ports or acquiring a device session. Serial board/version fields remain unknown even when a USB product string resembles a board name. USB VID/PID/product/serial are descriptor hints only: Espressif USJ, ESP-VISION USB-OTG candidates, USB-to-UART converters remain visible by default; native UART ports are available with --all-ports. ROM/unflashed candidates are not discarded. Firmware cannot be inferred from an unanswered probe. A USB device exposing no OS serial port cannot be enumerated by this serial backend; USB/ROM modes without a serial endpoint require a separate host USB adapter.

Filtering follows the OS enumeration, then checks platform names and USB descriptors; it never guesses ports by generating filenames or limits discovery to Espressif VID/PID. Windows accepts COM1 and higher (including COM10+ and extended device paths); Linux normally exposes USB serial ports as /dev/ttyACM*, /dev/ttyUSB* or /dev/ttyXRUSB*; macOS uses /dev/cu.* and /dev/tty.*, preferring cu.* when both endpoints exist. USB descriptors also retain custom driver names. Missing board identity or firmware does not remove a USB candidate.

By default, only USB serial candidates appear. --all-ports additionally shows OS-enumerated native UART, Bluetooth and other serial ports: ttyS*, ttyAMA*, ttyTHS* and rfcomm* are legitimate Linux ports, not invalid names. Windows COM names alone cannot distinguish USB from native/virtual serial; without USB descriptors they require --all-ports. macOS Bluetooth/debug ports likewise require --all-ports. An explicit --port bypasses discovery filtering, including Linux /dev/serial/by-id/... aliases. Neither mode opens ports just to test their presence.

The rules follow `pySerial enumeration <https://pyserial.readthedocs.io/en/latest/tools.html#serial.tools.list_ports.comports>`_, `Microsoft serial handles <https://learn.microsoft.com/en-us/windows/win32/devio/communications-resource-handles>`_, `Linux serial device names <https://www.kernel.org/doc/html/latest/admin-guide/devices.html>`_ and `Apple serial device access <https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/WorkingWSerial/WWSerial_SerialDevs/SerialDevices.html>`_. The CLI command inspect matches the JavaScript method device.inspect().

.. code-block:: console

   esp-vision-device inspect --json
   esp-vision-device inspect --interface serial
   esp-vision-device inspect --interface serial --all-ports
   esp-vision-device inspect --interface usj,usb --probe
   esp-vision-device inspect --interface tcp
   esp-vision-device inspect --select 1 --fields identity,capabilities

Results have per-invocation numbers. Identified devices sort before LAN advertisements, unprobed ports and failed probes. Successful identification assigns a shared group to the same deviceId across interfaces, preserving each endpoint. Unidentified/busy/inaccessible ports remain present with error codes; a failed probe does not prove firmware is absent. USB serial numbers are displayed independently from firmware deviceId. --select uses the same enumeration snapshot in that invocation, then connects its selected endpoint; scripts should use an explicit address. A LAN advertisement is not proof of current reachability.

--probe explicitly connects and can interrupt Python. Probe timeout defaults to 5000 ms per endpoint; --probe-timeout changes it. --interface restricts enumeration to all/serial/usb/usj/uart/tcp (comma-separated); serial includes every wired port admitted by the enumeration policy. Default LAN collection is 3000 ms, bounded to 10000 ms. Platform-specific paths are accepted without rewriting.

.. code-block:: console

   esp-vision-device inspect --port COM3
   esp-vision-device inspect --port /dev/ttyACM0 --role usj
   esp-vision-device inspect --port /dev/cu.usbmodem1101 --role usb
   esp-vision-device inspect --tcp 192.168.1.100:3232
   esp-vision-device inspect --tcp esp-vision-board.local:3232 --fields identity,capabilities,runtime,network,storage --json

A target always opens a Core session, queries and releases it. Default fields are identity/capabilities/runtime; add network/storage as needed. --fields without a target requires --probe or --select. --role is optional for explicit serial targets (default unknown); selecting a discovered descriptor candidate uses its candidate role. Port/IP identification still relies on the firmware negotiation. Hostname TCP connections depend on host DNS/mDNS resolution; use the discovered IP if .local resolution is unavailable. TCP supports bracketed IPv6 addresses but current DNS-SD records supply IPv4 endpoints. No claim of passive protocol probing is made.


CLI and SDK naming
----------------------------------------

Command names match SDK methods: inspect, configure, control and firmware check/update. CLI options use kebab-case and JavaScript uses camelCase or nested objects; this changes syntax, not the underlying operation. The table omits transport selection and cleanup for brevity.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - CLI
     - SDK
   * - ``inspect --fields identity,network``
     - ``device.inspect({ fields: ["identity", "network"] })``
   * - ``configure --network-enabled true``
     - ``device.configure({ network: { enabled: true } })``
   * - ``configure --ssid NAME``
     - ``device.configure({ network: { credentials: { ssid, password } } })``
   * - ``configure --forget-wifi``
     - ``device.configure({ network: { credentials: null } })``
   * - ``control --action reset --mode soft``
     - ``device.control({ action: "reset", mode: "soft" })``
   * - ``firmware check --version VERSION``
     - ``device.firmware.check({ version })``
   * - ``firmware update --version VERSION``
     - ``device.firmware.check({ version })`` → ``device.firmware.update({ release, openLoader })``

Without a target, CLI inspect is a host inventory operation that combines OS serial enumeration with LAN discovery; it does not call device.inspect() on a disconnected device. SDK device.discover() lists LAN advertisements through its discovery adapter, while device.inspect() queries an established session. --port/--tcp select the host transport; --probe/--select/--all-ports/--json are CLI presentation or discovery options, not additional SDK API names. --ssid obtains the password from a prompt/stdin; configuration output intentionally shows only ok, avoiding credential disclosure. --image maps local bytes to firmware.update data, while --version first selects a release through firmware.check.

CLI --timeout defaults to 30000 ms for explicit targets, overriding the SDK method defaults when forwarded to connect/inspect/configure/control; it is not an aggregate deadline for the entire command. Firmware check/update use cancellation signals rather than this timeout for download/flashing. SDK configuration defaults to 45000 ms and control to 15000 ms when called directly without a timeout.

Configuration and control
----------------------------------------

.. code-block:: console

   esp-vision-device configure --port COM3 --ssid MyWiFi
   esp-vision-device configure --port /dev/ttyACM0 --ssid MyWiFi --password-stdin
   esp-vision-device configure --port /dev/ttyACM0 --network-enabled false
   esp-vision-device configure --port /dev/ttyACM0 --forget-wifi
   esp-vision-device control --tcp 192.168.1.100 --action stop
   esp-vision-device control --port COM3 --action reset --mode soft
   esp-vision-device control --port COM3 --action reset --mode hard
   esp-vision-device control --port COM3 --action bootloader

SSID configuration prompts for a hidden password; --password-stdin reads it from standard input (one final newline is stripped). There is no command-line password argument. Configuration outputs only ok:true, never credentials. An empty password selects an open network. Wi-Fi changes can break TCP, so prefer serial provisioning. Control reset requires an explicit soft/hard mode; successful reset/download acknowledgement does not imply a reconnected device. Enumeration never resets or writes a device.

Firmware management
----------------------------------------

.. code-block:: console

   esp-vision-device firmware check --port COM3
   esp-vision-device firmware check --port COM3 --version 2026.10.15
   esp-vision-device firmware check --board ESP32_P4X_EYE --json
   esp-vision-device firmware update --port COM3 --version 2026.10.15

check reads https://dl.espressif.com/AE/esp-vision/config.toml automatically, then fetches per-board/per-version metadata under firmware/<board>/<version>.json. It returns all listed versions; historical versions without qualified update metadata remain visible with compatible:false and reason UPDATE_METADATA_UNAVAILABLE, and cannot be flashed through this application-only update flow. --version selects an exact qualified version. --catalog is an optional advanced override, not a required input. Offline checks need only --board; --current-version is optional and defaults to unknown.

update requires a local --port and explicit --version. The built-in serial flasher internally calls separately installed Python esptool>=5.3,<6; install it once with python -m pip install "esptool>=5.3,<6". No JavaScript loader module or --loader argument is accepted. --python selects the interpreter, not a user adapter. An explicit --board allows qualified recovery without acquiring a live v4 session, but the physical chip/security/partition checks still apply. TCP is not OTA. Image download, application-only flashing, complete readback and reset handling reuse Core's update API. --image is an advanced local-bin override.

The firmware release pipeline generates application bins plus SHA-256, actual partition bounds/table hashes and image-header silicon revision ranges from built artifacts. It publishes them additively beside the existing merged full-flash images; the website's existing flashing flow is unchanged. The six legacy releases present during this implementation lack this metadata; they can be listed, but no qualified official upgrade is available until the new firmware release is published. No fake successful update or guessed compatibility is reported.

Output and cancellation
----------------------------------------

--json writes one result object to stdout; default enumeration uses spaces padded to Unicode display widths, with numbered device blocks on narrow terminals and a column-aligned table when it fits, and other operations print formatted JSON. Partial enumeration warnings go to stderr (or the JSON warnings array). If all enumeration sources fail, the command fails; a successful empty source is a valid empty list. Errors are bounded code/message objects on stderr with a nonzero exit code, without raw device tracebacks or configuration data. Ctrl-C/SIGTERM cancels discovery and Core operations and releases resources; Core deliberately completes verification once flash writing starts. Terminal control characters in tabular device data are escaped. Current live-device coverage is Linux; Windows/macOS argument/descriptor behavior is covered by automated tests, not a claim of completed hardware tests on those systems.

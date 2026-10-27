Device management CLI
========================================

:link_to_translation:`zh_CN:[中文]`

The standalone ``@esp-vision/device-cli`` npm package installs ``esp-vision-device``. The CLI uses the public ``@esp-vision/device-core`` API and supports discovery, device management, Python execution, device files, capture, batch plans, and bounded event watches. Requires Node.js 22 or newer.

Installation and host requirements
----------------------------------------

.. code-block:: console

   npm install -g @esp-vision/device-cli
   esp-vision-device --help
   esp-vision-device inspect

Serial access requires Python with ``pyserial``. Windows defaults to ``python`` and macOS/Linux to ``python3``; use ``--python`` to select another interpreter. Firmware flashing also requires ``esptool>=5.3,<6`` in that interpreter. LAN discovery uses the CLI's bundled multicast-DNS client.

Discovery and inspection
----------------------------------------

Without a target, ``inspect`` enumerates OS serial ports and LAN advertisements without opening serial ports or acquiring device sessions. USB descriptors are candidate hints, not firmware identity. USB serial candidates are shown by default; ``--all-ports`` includes other OS-enumerated serial ports. ``--interface`` accepts comma-separated ``all``, ``serial``, ``usb``, ``usj``, ``uart``, and ``tcp`` filters. An explicit ``--port`` bypasses discovery filtering. Linux ``/dev/serial/by-id/...`` aliases are accepted.

.. code-block:: console

   esp-vision-device inspect --json
   esp-vision-device inspect --interface serial --all-ports
   esp-vision-device inspect --interface usj,usb --probe
   esp-vision-device inspect --select 1 --fields identity,capabilities,runtime
   esp-vision-device inspect --port /dev/ttyACM0 --role usj
   esp-vision-device inspect --tcp esp-vision-board.local:3232 --fields identity,network,storage

``--probe`` queries discovered candidates and ``--select`` connects to the selected result from that invocation. Both use passive EV-MUX acquisition; they do not send Ctrl-C to the running application. ``--probe-timeout`` defaults to 5000 ms per candidate and accepts up to 60000 ms. Inventory collection defaults to 3000 ms; LAN collection is capped at 10000 ms. Targeted inspection defaults to identity, capabilities, and runtime; ``--fields`` also accepts ``network`` and ``storage``. A targeted inspect, configure, firmware check, capture, watch, and native ``files write`` use passive acquisition. Python execution, REPL-backed ``files list/read/remove/mkdir``, control, firmware update, and all batch plans use the normal REPL connection probe and can send Ctrl-C. For an explicit serial target, ``--role`` accepts ``usj``, ``usb``, ``uart``, or ``unknown`` and requires ``--port``; the default is ``unknown``.

Runtime fields ``scriptRunning`` and ``run`` reflect work tracked by the current SDK session. A fresh CLI process cannot use those fields to determine whether a detached program is still running on the device. ``vmReady`` means the VM is ready; it does not mean the VM is idle.

Configuration, control, and firmware
----------------------------------------

.. code-block:: console

   esp-vision-device configure --port COM3 --ssid MyWiFi
   esp-vision-device configure --port COM3 --ssid MyWiFi --password-stdin
   esp-vision-device configure --port COM3 --network-enabled false
   esp-vision-device configure --port COM3 --forget-wifi
   esp-vision-device control --tcp 192.168.1.100 --action stop
   esp-vision-device control --port COM3 --action reset --mode soft
   esp-vision-device control --port COM3 --action bootloader
   esp-vision-device firmware check --port COM3 --version 2026.10.15
   esp-vision-device firmware check --board ESP32_P4X_EYE --json
   esp-vision-device firmware update --port COM3 --version 2026.10.15

Configuration prompts for a hidden Wi-Fi password; ``--password-stdin`` reads it from standard input. There is no command-line password option. Configuration output does not include credentials. Reset requires ``--mode soft`` or ``--mode hard``; an acknowledgement does not mean the device has reconnected.

Firmware checks use the official feed by default; ``--catalog`` accepts a local catalog file or HTTPS URL. An offline check needs ``--board`` and optionally accepts ``--current-version``. Update requires a local ``--port`` and explicit ``--version``; the built-in serial flasher uses ``esptool>=5.3,<6``. TCP is not OTA. The default ``--mode update`` writes the application and preserves the existing layout and user data. ``--mode install`` requires ``--board`` and writes a complete image, replacing data in its written range; use a ROM-capable USJ/UART port for blank-board installation. ``--image`` supplies local firmware bytes and does not bypass release integrity checks.

Python execution
----------------------------------------

Choose exactly one Python input: inline ``--source CODE``, local ``--file LOCAL.py``, or device ``--path DEVICE.py``. ``--completion wait`` is the default and waits for completion. A positive ``--timeout MS`` cancels a waiting execution when it expires. ``--max-output-chars`` bounds captured output (16,384 by default, up to 1 MiB).

.. code-block:: console

   esp-vision-device python --source 'print(42)' --port COM3
   esp-vision-device python --file ./main.py --port COM3 --timeout 30000
   esp-vision-device python --path /main.py --completion start --port COM3

``--completion start`` accepts only ``--path`` and acknowledges after the device VM enters its runner. This does not confirm application initialization or completion. The CLI releases the connection and exits; it does not keep watching or resume the script. EV-MUX pins Python stdout to the session that started the program. Use foreground ``--completion wait`` for live Python output; a later watch cannot reattach to a detached run's stdout. Use captures or device-written progress artifacts to verify a detached application.

Foreground Python execution and Python steps in a batch stream ``python.output``, ``python.log``, and ``python.state`` NDJSON events on stderr. Output is bounded by ``--max-output-chars`` with one truncation marker. The stdout result contains metadata, not a duplicate of streamed Python text.

Device files and capture
----------------------------------------

Device paths such as ``/models`` belong to the device, including on Windows. Invalid paths are rejected before connecting. Git Bash/MSYS can rewrite slash-prefixed arguments to Windows host paths; use PowerShell or prefix the individual command with ``MSYS2_ARG_CONV_EXCL="*"`` and supply verified Windows-form local paths. Quoting ``/models`` alone does not prevent shell conversion. Do not strip a drive prefix from a converted path or retry device operations blindly.

.. code-block:: console

   esp-vision-device files list --path / --recursive --port COM3
   esp-vision-device files read /main.py --output ./main.py --port COM3
   esp-vision-device files write /backup.bin --file ./backup.bin --port COM3
   esp-vision-device files remove /old.bin --type file --port COM3
   esp-vision-device files mkdir /data --recursive --port COM3
   esp-vision-device capture camera --output ./frame.jpg --quality 80 --port COM3
   esp-vision-device capture screen --output ./screen.jpg --port COM3

File reads and JPEG captures write binary data to the requested local ``--output`` and return path, byte count, and metadata. Files read/capture are capped at 16 MiB by default; ``--max-bytes`` may lower the cap. ``files write`` sends local bytes with the binary file RPC. Images and file contents are not embedded in JSON.

Batch and watch
----------------------------------------

``batch PLAN.json`` validates the entire ``schemaVersion: 1`` plan before connecting, executes operations sequentially over one connection, and stops at the first failure. A report lists completed operations and the failure. Supported operations are ``python``, ``files.list``, ``files.read``, ``files.write``, ``files.remove``, ``files.mkdir``, and ``capture``. Python steps wait for completion. Local file paths are relative to the plan file; device paths are absolute. Batch uses the normal REPL connection probe because a plan may run Python or REPL-backed file operations.

.. code-block:: json

   {
     "schemaVersion": 1,
     "operations": [
       { "op": "python", "source": "print('hello')" },
       { "op": "files.read", "path": "/boot.py", "output": "./boot.py" },
       { "op": "capture", "source": "screen", "output": "./screen.jpg" }
     ]
   }

``watch --duration MS`` requires ``--port`` or ``--tcp`` and a duration from 1 ms to 1 hour. It passively observes output, logs, state changes, and diagnostics available to the currently acquired session, then releases the connection. Watch does not launch scripts, reconnect, or reattach to a prior detached run's Python stdout. SIGINT/SIGTERM cancel the command and release ownership.

Output, timeouts, and cancellation
----------------------------------------

``--json`` writes one ``schemaVersion: 1`` JSON envelope to stdout. Status, Python output, logs, firmware phases, cancellation, errors, and duration are NDJSON events on stderr. Binary content is always written to local files. ``--timeout`` accepts 1..600000 ms. It applies to inspect/configure/control and inventory collection; ``--probe-timeout`` sets the timeout for each discovered candidate probe. When explicitly supplied, ``--timeout`` also applies to foreground Python, file reads, captures, and those operations in a batch. It is not a connection timeout: device connection setup uses 30000 ms. Python ``--completion start`` rejects ``--timeout``; watch uses ``--duration``; firmware operations do not use this option but support signal cancellation. During firmware update, once writing begins, Core completes readback verification and reset handling. File list/write/remove/mkdir support signal cancellation without a per-operation timeout. SIGINT exits with code 130, SIGTERM with 143, argument/plan errors with 2, other failures with 1, and success with 0. Busy or uncertain operations are not retried automatically. See ``esp-vision-device --help --json`` for the machine-readable command and capability reference.

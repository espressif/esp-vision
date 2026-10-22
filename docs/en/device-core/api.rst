Application API reference
==================================================

:link_to_translation:`zh_CN:[中文]`

This page covers the complete object returned by ``createDevice`` in package 0.4.0 (prepared, not published by this change). Unless stated otherwise, operations are asynchronous and reject on failure. Unknown request fields are rejected. Do not add an undocumented option such as ``timeout`` to every method: accepted keys are listed per method. Millisecond timeouts are positive finite numbers.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Method
     - Purpose
   * - ``createDevice(options)``
     - Create one independently managed device object.
   * - ``device.discover(options)``
     - List LAN advertisements without acquiring ownership.
   * - ``device.connect(options)``
     - Open/negotiate a v4 connection and probe REPL readiness.
   * - ``device.disconnect()``
     - Release ownership, cancel work and close transport.
   * - ``device.inspect(request)``
     - Read identity, capabilities, runtime, network or storage.
   * - ``device.configure(request)``
     - Apply a network configuration patch.
   * - ``device.control(request)``
     - Stop, reset or enter ROM download mode.
   * - ``device.firmware.check(request)``
     - Check the recommended firmware for the board.
   * - ``device.firmware.update(request)``
     - Download, qualify, flash and verify application or complete-install images.
   * - ``device.capture(request)``
     - Capture camera or LCD JPEG.
   * - ``device.on(event, handler)``
     - Subscribe and obtain an unsubscribe function.
   * - ``device.python.input(data)``
     - Send interactive REPL/stdin input.
   * - ``device.python.execute(request)``
     - Execute source or an existing device script.
   * - ``device.files.list(request)``
     - List a directory.
   * - ``device.files.read(request)``
     - Read bytes or UTF-8 text.
   * - ``device.files.write(request)``
     - Create/overwrite a text or binary file.
   * - ``device.files.remove(request)``
     - Remove one file or empty directory.
   * - ``device.files.mkdir(request)``
     - Create a directory and optional parents.

Creation and byte transport
------------------------------------------------------

``createDevice({ transport?, discovery? } = {}) → device``

``transport`` supplies raw bytes; ``discovery`` supplies ``list({ timeout, signal }) → Promise<Array>``. Either can be omitted at construction; a transport is required at connection time. A discovery-only instance may receive a transport in ``connect``. Core does not choose a serial port or create a socket from a discovery record automatically.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Transport member
     - Contract
   * - ``writeBytes(bytes)``
     - Required; accepts Uint8Array, returns a Promise after all bytes are accepted by the host connection. Reject short/failed writes.
   * - ``subscribe({ data, error, close })``
     - Required; synchronously returns an unsubscribe function. data receives original Uint8Array chunks.
   * - ``connect({ signal })``
     - Optional; opens the connection after Core subscribes.
   * - ``disconnect()``
     - Optional; closes the physical connection and releases reader/writer resources.

device.discover(options)
------------------------------------------------

``{ timeout = 3000, signal? } → Promise<DiscoveredDevice[]>``

``timeout`` is an integer from 50 to 10000. A discovery provider is mandatory; missing configuration throws ``DISCOVERY_UNAVAILABLE``. Results are normalized and sorted by hostname; at most 256 records are accepted. Discovery neither connects nor determines whether an owner is active.

.. code-block:: javascript

   const discovered = {
     deviceId: 'e8f60ae75642',
     hostname: 'esp-vision-esp32-p4x-eye-e75642-2026-08-27',
     board: 'ESP32_P4X_EYE', firmwareVersion: '2026.08.27',
     evMuxVersion: 4, compatible: true, port: 3232,
     addresses: ['192.168.1.107'],
     // Optional: connectionUrl (bridge), expiresAt (expiry timestamp).
   }

The example is illustrative metadata, not a device to connect to. ``deviceId`` is the main-chip MAC without separators. ``firmwareVersion`` is the firmware version, not the IDE version. Build a byte transport using the selected address/port, then pass the record as ``expectedDevice``. Node and browser providers are documented in :doc:`advanced`.

device.connect(options)
----------------------------------------------

``{ transport?, role = "unknown", expectedDevice = null, timeout = 30000, probeRepl = true, signal? } → Promise<Inspection>``

``role`` accepts ``unknown``, ``usj``, ``usb``, ``uart`` or ``tcp``. Core subscribes before opening transport, negotiates v4, checks ``expectedDevice`` when provided, acquires a session and probes the REPL. The result is the default inspection (identity/capabilities/runtime). An already connecting, connected or disconnecting instance rejects with ``DEVICE_BUSY``. Identity matching is not authentication. The default ``probeRepl: true`` sends an interrupt and probes the REPL, which can stop an existing Python program. Set ``probeRepl: false`` to acquire the EV-MUX session without sending REPL input, for observation or native RPC operations while a program continues running. This still acquires the device session and respects ownership; it is not a force-takeover option. Python execution and REPL-backed file operations can subsequently interrupt the program. The option must be a boolean.

device.disconnect()
--------------------------------------

Returns a Promise. It releases the session, aborts pending work, closes the session interpreter, unsubscribes input and calls the optional transport close. Repeated calls share cleanup; an already disconnected object resolves without work. Release/transport cleanup failures may reject, so use ``finally`` even when connect fails. Reconnection requires a transport that can reopen or a new transport supplied to ``connect``.

device.inspect(request)
----------------------------------------------

``{ fields = ["identity", "capabilities", "runtime"], timeout = 5000, signal? } → Promise<Inspection>``

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Field
     - Result
   * - ``identity``
     - ``{ deviceId, hostname, board, firmwareVersion } | null``
   * - ``capabilities``
     - Negotiated firmware capabilities, features, channels and limits.
   * - ``runtime``
     - Session/parser/transport snapshot plus vmReady, vmEpoch and current run or null. ``scriptRunning`` and ``run`` track executions started by this SDK session; a fresh connection cannot use false/null to establish that a detached program is idle. ``vmReady`` means the VM has started, not that it is idle.
   * - ``network``
     - network.status reply including enabled/configured/state/IPv4 and operation state; no saved password.
   * - ``storage``
     - flash and sd storage.writer.status replies; requires storage.writer.lease.

``fields`` must be a nonempty array of the five names above. Only requested keys are returned; duplicate fields are queried once. Identity/capabilities are copied negotiated data, not a new sensor sample. Network and storage perform RPCs; the supplied timeout applies to each underlying query, not a single aggregate inspection deadline.

device.configure(request)
--------------------------------------------------

``{ network: { enabled?, credentials? }, timeout = 45000, signal? } → Promise<{ ok, completed, network }>``

``credentials`` is ``{ ssid, password }`` or ``null``. SSID is 1–32 UTF-8 bytes without NUL. Password is empty for an open network, 8–63 printable ASCII characters, or a 64-digit hexadecimal PSK. ``enabled`` is boolean. Supply at least one patch field; omitted fields preserve existing settings. Clearing credentials disables the network and cannot be combined with ``enabled: true``.

.. code-block:: javascript

   await device.configure({ network: {
     credentials: { ssid: 'example', password: 'example-password' },
     enabled: true,
   } })
   await device.configure({ network: { enabled: false } })
   await device.configure({ network: { credentials: null } })

The total configuration deadline includes polling operation completion, not only the accepted ACK. ``completed`` contains acknowledged ``{ action, operationId }`` steps. Failure attaches ``error.completed`` and optionally ``error.pending``; already completed steps are not rolled back. The patch is a sequence, not an atomic transaction. Prefer USB provisioning when Wi-Fi changes can drop TCP. Never log credentials.

device.control(request)
----------------------------------------------

``{ action, mode?, timeout = 15000, signal? } → Promise<control result>``

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Request
     - Behavior
   * - ``{ action: "stop" }``
     - Interrupt/stop Python; usable while python.execute is active.
   * - ``{ action: "reset", mode: "soft" }``
     - Soft-reset the VM; observe the new epoch and readiness.
   * - ``{ action: "reset", mode: "hard" }``
     - Restart hardware; the connection may disappear.
   * - ``{ action: "bootloader" }``
     - Request ROM download mode; disconnect and use a ROM loader.

``mode`` is mandatory only for reset and rejected for other actions. A stop acknowledgement does not prove the running script has finished cleanup: observe its ``done`` result. Reset/download ACKs do not prove normal boot or download enumeration; reconnect and inspect after the device returns.

device.python.input(data)
--------------------------------------------------

Accepts a string or Uint8Array, returns the underlying write Promise. Send ``\r`` to submit an interactive line. During a ``completion: "start"`` run, it writes stdin (for example Python ``input()``). It has no request-object timeout/signal arguments. Output is delivered by ``python.output``; writing input alone does not prove evaluation succeeded. Other automatic operations reserve the interpreter and reject conflicting terminal input with ``DEVICE_BUSY``.

device.python.execute(request)
------------------------------------------------------------

``{ source? | path?, completion = "wait", timeout = null, maxOutputChars?, signal? }``

Provide exactly one of ``source`` (string) or ``path`` (absolute device file). With ``completion: "wait"``, resolves ``{ ok, path, output, outputTruncated }``; inline source has ``path: null``. With ``completion: "start"``, resolves ``{ ok: true, path, done }`` and ``done`` resolves the same result shape. The start result follows a private VM-entry marker from the generated runner; it does not confirm application initialization or completion. ``timeout`` is accepted only for wait mode; omission or null means no aggregate execution deadline, while setup exchanges remain bounded. ``maxOutputChars`` is a nonnegative integer controlling retained output, not UART bandwidth or script lifetime.

.. code-block:: javascript

   const off = device.on('python.output', text => terminal.write(text))
   try {
     const run = await device.python.execute({
       source: 'name = input("Name? "); print(name)', completion: 'start',
     })
     await device.python.input('example\r')
     console.log(await run.done)
   } finally { off() }

Here ``terminal`` is supplied by the UI. Start mode uploads source to a unique Core-owned temporary file and removes it after completion; it does not remove a user-supplied path. After the start handle returns, ``device.disconnect()`` releases the session without requesting a stop; ``done`` rejects locally as the session closes, and the device program may continue. The request's ``signal`` remains bound until ``done`` settles and requests a stop if aborted while the session is available. Disconnect can prevent temporary-file cleanup. Python stdout remains tied to the original EV-MUX session and cannot be reattached from a new connection; keep the session open for live output and use captures or application progress artifacts to check a detached program. Inspect the returned output for Python tracebacks: ``ok`` is an execution/transport result and is not a universal assertion that arbitrary Python code produced a correct application result.

Device file operations
--------------------------------------------

Device paths must be absolute and at most 123 UTF-8 bytes. Control characters, backslash, double quote, ``..`` anywhere, repeated slash and a trailing slash are rejected. Only list accepts root ``/``. Filesystem mount/storage ownership and free space must permit the operation. Methods below reserve the same interpreter used by Python.

device.files.list(request = {})
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``{ path = "/", recursive = false, signal? }``

Returns a flat array of { name, path, type: "file" | "directory", size }; directory size is zero. Recursive results remain flat.

device.files.read(request)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``{ path, encoding = "binary", timeout = null, chunkTimeout = 30000, maxBytes = null, signal? }``

Returns Uint8Array for binary or string for utf-8. Omitted/null ``timeout`` means no aggregate read deadline; ``chunkTimeout`` bounds each REPL exchange and defaults to 30000 ms. An explicit positive total timeout caps each exchange by the remaining time. maxBytes is null or an integer 0..16777216 and bounds the complete file read; exceeding it rejects instead of returning a silent prefix.

device.files.write(request)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``{ path, data, overwrite = true, onProgress = null, signal? }``

data is string or Uint8Array. overwrite:false requests create-only. Progress receives { writtenBytes, totalBytes }. Returns the verified commit acknowledgement with total bytes, complete:true and path, after the firmware checks the complete SHA-256. It does not execute the file.

device.files.remove(request)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``{ path, type = "file", signal? }``

type is file or directory; directory removal requires an empty directory. Returns Promise<void>. There is no recursive remove flag.

device.files.mkdir(request)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``{ path, recursive = false, signal? }``

Returns {ok:true}; recursive mode creates parents and accepts an existing directory, but not an existing file in its place.

SDK 0.2.0 and later transport all file types through reliable raw binary ``file.write`` frames, including Python source and model files. Upload requires firmware feature ``file.write.binary`` and a valid ``limits.fileWriteChunkMax`` (4096..16384, aligned to 4096); missing support rejects with UNSUPPORTED_OPERATION and a firmware-upgrade message. There is no Base64/JSON or REPL upload fallback. list/read/remove/mkdir still use framed REPL; no separate public files.writeBinary method is needed. Progress reports confirmed bytes, not a durable commit. Only the final resolved result means length/hash verification and file publication completed. Cancellation or failure triggers best-effort abort; firmware also expires stale transfers and cleans up on session/VM changes. Existing target contents survive upload, hash and ordinary pre-publication failures. FAT replacement uses an old-file backup and rollback for ordinary rename failure; it is not a power-loss-atomic journal. See :doc:`../communication/python-files` for the wire contract. Application updates use ``device.firmware.update()`` and reuse the ROM implementation in the ``/firmware`` subpath.

device.capture(request)
----------------------------------------------

``{ source, format = "jpeg", quality?, timeout = 15000, signal? } → Promise<{ data, mimeType, metadata }>``

``source`` is ``camera`` or ``screen``; the only format is ``jpeg``. Optional quality is an integer 1–100; omission uses the firmware default. data is Uint8Array, mimeType is image/jpeg, and dimensions are in metadata.width/height (not top-level width/height).

.. code-block:: javascript

   const frame = await device.capture({ source: 'screen', quality: 80 })
   console.log(frame.metadata.width, frame.metadata.height, frame.data.length)
   const blob = new Blob([frame.data], { type: frame.mimeType })

Camera capture initializes/uses the camera in the VM. Screen capture requires an initialized LCD and reads its last software-submitted RGB565 frame without starting a camera or redrawing the screen. Unsupported hardware reports NOT_SUPPORTED; uninitialized LCD reports DISPLAY_NOT_READY. Screenshots do not read back the physical panel. See :doc:`preview-debug <../communication/preview-debug>` for lazy frame storage, DMA completion and preview delivery.

device.on(event, handler)
--------------------------------------------------

Synchronous; returns ``unsubscribe()``. Subscribe before connect when startup output is needed. Throwing listeners do not interrupt parsing or other listeners; a diagnostic listener.error is emitted. Remove subscriptions and revoke image object URLs when disposing UI.

.. list-table::
   :header-rows: 1
   :widths: 25 25 50

   * - Event
     - Payload
     - Meaning
   * - ``python.output``
     - ``string``
     - Python stdout/stderr, prompts and interactive output.
   * - ``raw``
     - ``string``
     - Text outside EV-MUX frames, including early boot output.
   * - ``log``
     - ``string``
     - Firmware/IDF diagnostic text.
   * - ``preview``
     - ``{ data, mimeType, metadata }``
     - JPEG from application img.flush(); delivery may drop frames.
   * - ``state``
     - ``{ state, identity?, reason? }``
     - connecting/ready/disconnected and hello/network.changed hints; identity is provided on ready.
   * - ``diagnostic``
     - ``{ event, ...details }``
     - rpc.request/response/timeout, listener.error and cleanup diagnostics; sensitive network credentials are redacted.
   * - ``rpc``
     - ``{ channel, metadata, payload }``
     - Advanced inbound RPC observation; prefer typed device methods for operations.

Concurrency, cancellation and errors
------------------------------------------------------------------------

Inspection, configuration, control, Python, file and capture methods require a ready connection. Discovery, event subscription and firmware checks do not; firmware update can also run disconnected with an explicit board and ROM loader. The facade serializes exclusive Python/file/configuration/capture operations by rejecting conflicts with DEVICE_BUSY; it does not silently enqueue arbitrary application work. stop/reset can coordinate with an active python.execute. A started run keeps its reservation until done; interactive input is allowed while that run accepts stdin.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Error
     - Handling
   * - ``TypeError / RangeError / INVALID_PATH``
     - Correct parameters; requests reject unknown keys, unsupported enums and invalid paths.
   * - ``DEVICE_NOT_READY / DEVICE_BUSY``
     - Connect first, or await/stop the current operation.
   * - ``DISCOVERY_UNAVAILABLE / UNSUPPORTED_OPERATION``
     - Supply a provider or use compatible firmware capabilities.
   * - ``SESSION_BUSY / SESSION_LOST / VERSION_MISMATCH``
     - Release another valid owner, reconnect after loss, or update firmware; do not force takeover or retry writes blindly.
   * - ``TIMEOUT / AbortError / CONNECTION_CLOSED``
     - AbortSignal may preserve its supplied Error reason. Completion can be unknown; verify identity/state/files before retrying changes.
   * - ``VM_BUSY / VM_TIMEOUT / DISPLAY_NOT_READY / NOT_SUPPORTED``
     - Handle firmware errors explicitly. Stop/initialize the required resource or report unavailable hardware.

All asynchronous callers must catch rejections; errors may include code, errno, completed and pending. Not every error class supplies a code. Disconnect/cancel does not roll back device changes, guarantee script termination, or authorize retrying a non-idempotent operation.

Firmware checks and updates
------------------------------------------

``device.firmware.check({ catalog?, identity?, version?, mode = 'update', fetch = globalThis.fetch, signal? })``

Returns ``{ available, currentVersion, release, releases }``. Uses the connected device identity; when disconnected, pass an inspected ``identity`` containing ``board`` and ``firmwareVersion``. Checking performs no connection acquisition, reset or writes. ``catalog`` is either an in-memory object or an absolute HTTPS JSON URL. Fetch errors reject; no matching board returns ``release: null, available: false``. Without catalog, Core reads the official dl.espressif.com ESP-VISION index and per-version update metadata; a custom catalog remains an advanced override. Unqualified historical entries remain visible but cannot be selected for installation.

Catalog shape: ``{ schemaVersion: 1, releases: [{ board, version, image }] }``. The publisher orders entries newest first; By default Core selects the first entry matching the board and validates its v4 image metadata; an explicit version overrides this selection. ``available`` means its version differs from the current firmware, not that an arbitrary calendar/git label compares numerically greater. The feed publisher is responsible for the recommended release order; the UI must show the target version before an update, including any intended downgrade.

Each ``image`` contains ``evMuxVersion: 4``, non-negative integer ``chipFamily``, ``offset`` plus either exact ``chipRevision`` or integer ``chipRevisionMin``/``chipRevisionMax`` bounds, positive integer ``bytes`` and ``partitionSize`` (bytes must fit), lowercase 64-character ``sha256``, and ``partitionTable: { offset, bytes, sha256 }``. When downloading rather than supplying bytes, add an absolute HTTPS ``url`` pointing directly to the application bin. A zip/artifact download is not a raw bin and is not extracted by Core. Board names must exactly match the firmware identity. Version labels refer to firmware, not SDK/IDE versions. SHA-256 detects mismatch against the selected feed; it does not authenticate a malicious feed.

``device.firmware.update({ release, board?, mode = 'update', data?, fetch = globalThis.fetch, openLoader, reset?, onProgress?, onPhase?, signal? })``

``release`` is a selected check result entry. A connected device must match its board; without a connection, explicitly pass the physically selected ``board``. Without ``image.source``, ``data`` is a Uint8Array application BIN; otherwise Core downloads ``release.image.url``. With ``image.source``, both downloaded and supplied ``data`` are the complete BIN container; Core validates and extracts the application internally as described below. Core validates size/hash before releasing the EV-MUX connection, then calls host ``openLoader({ signal })`` to acquire the ROM download port. The host adapter handles serial/WebUSB selection and ROM entry; Core does not implement network OTA or invent a second download-mode wire command. Use a local download connection even when discovery/inspection used TCP.

The loader contract is the existing :doc:`advanced` FirmwareFlashSession contract. Optional ``reset(loader)`` replaces its hard-reset policy; ``onProgress(percent)`` reports 0–100 including readback and ``onPhase(text)`` reports phases. In default update mode only the application partition is written: chip/revision, security mode and the existing partition table are checked before writing, then the whole application is read back and hashed. Partition mismatch rejects; default update mode does not replace the partition layout or erase NVS/user files. Explicit installation is described below.

Returns ``{ verified: true, needsReset, version, board, reconnected: false }`` only after complete readback verification. ``needsReset: true`` means the host must ask for a physical reset. No automatic EV-MUX reconnection or running-version claim is made: reconnect and inspect the firmware version separately. Cancellation applies before download/write, including loader acquisition; after the flash write starts it is deliberately deferred so verification can finish. A failed update rejects and closes the acquired loader; rollback is not promised. Connection attempts, other device operations and concurrent updates are rejected with DEVICE_BUSY during an update. Unknown request fields reject. The device remains disconnected after handoff, including failures after that point.

.. code-block:: javascript

   const result = await device.firmware.check({
     catalog: trustedCatalog, // Ordered release metadata from the host publisher.
     // For offline/ROM recovery only: identity: previouslyInspectedIdentity
   })
   if (result.available && userApprovedVersion === result.release.version) {
     const updated = await device.firmware.update({
       release: result.release,
       openLoader: ({ signal }) => host.openRomLoader({ signal }),
       onProgress: percent => console.log(percent),
     })
     console.log(updated) // Reconnect/inspect separately; prompt if needsReset.
   }

Complete installation on blank devices
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use the same two firmware methods with ``mode: 'install'``. No EV-MUX connection or running firmware is required. Choose the physical board explicitly, enter ROM download mode and select its ROM-capable USB-Serial-JTAG (USJ) or UART port. An application-provided USB-OTG CDC interface cannot be assumed on a blank board. The host still supplies the ROM adapter; CLI includes its Node/esptool adapter. This is local ROM flashing, not TCP OTA.

Default ``mode: 'update'`` retains the application-only contract. Core never changes to install mode automatically after a layout mismatch. Explicit install writes a qualified merged image at address zero, including bootloader, partition table and application. Data in that image's written range, including any NVS/filesystem content, can be replaced; data preservation and rollback are not promised. There is no implicit whole-chip erase. An installation is not an application update and hosts must label that distinction before calling it.

The existing schemaVersion 1 release entry gains optional ``install: { offset: 0, bytes, flashSize, sha256, url, chipRevisionMin, chipRevisionMax, bootloader: { offset, bytes, sha256 } }``. ``flashSize`` is the layout's required flash capacity in bytes. Bootloader revision bounds are intersected with the application's bounds by the release publisher. ``image`` remains the application descriptor; install validation checks that its application and partition-table hashes and the bootloader hash match their exact ranges inside the merged image. All ranges must fit, be ordered and not overlap. Future release pipelines generate this metadata from the existing full image and real IDF artifacts; historical images without approved installation metadata are never guessed compatible.

``firmware.check({ mode: 'install', ... })`` selects the first qualified installation release, or an exact requested version. App-only releases remain visible in ``releases``; selecting one explicitly rejects with FIRMWARE_INSTALL_UNAVAILABLE. Unknown modes reject. ``firmware.update({ mode: 'install', ... })`` validates all image bytes before releasing a session or opening a ROM port, then checks the actual chip, silicon revision, secure-boot/encryption state and detected JEDEC flash capacity before writing. It skips the existing partition-table comparison because a blank device has none. It uses the same complete readback, reset policy, exclusivity, cancellation and result shape as application updates. Reconnect and inspect to confirm running firmware separately.

.. code-block:: javascript

   const device = createDevice() // No runtime transport or connect() required.
   const selected = await device.firmware.check({
     identity: { board: 'ESP32_P4X_EYE', firmwareVersion: 'unknown' },
     mode: 'install', version: selectedFirmwareVersion,
   })
   await device.firmware.update({
     release: selected.release, board: 'ESP32_P4X_EYE', mode: 'install',
     openLoader: ({ signal }) => host.openRomLoader({ signal }),
   })

Selecting a firmware version
------------------------------------------

``releases`` contains all catalog entries for the current board, in publisher order. Without ``version``, ``release`` is the first recommended entry. Pass ``version`` to select an exact firmware version; a missing version rejects with FIRMWARE_VERSION_UNAVAILABLE, never falls back to another build. Duplicate board/version entries reject as ambiguous. The catalog determines which history is available; Core does not scrape all Git tags, CI artifacts or unpublished versions. Choosing an older version is explicit and uses the same update qualification.

.. code-block:: javascript

   const history = await device.firmware.check({ catalog: trustedCatalog })
   console.log(history.releases.map(item => item.version))
   const selected = await device.firmware.check({
     catalog: trustedCatalog, version: selectedFirmwareVersion,
   })
   await device.firmware.update({
     release: selected.release,
     openLoader: ({ signal }) => host.openRomLoader({ signal }),
   })

Node hosts can reuse the shipped ROM adapter directly:

.. code-block:: javascript

   import { openNodeRom } from '@esp-vision/device-core/node'
   await device.firmware.update({
     release: selected.release, board: 'ESP32_P4X_EYE', mode: 'install',
     openLoader: ({ signal }) => openNodeRom({ port: selectedRomPort, signal }),
   })

SDK 0.5.1 includes the browser ROM adapter ``openBrowserRom({ port, logger?, signal? })`` from ``@esp-vision/device-core/browser``. It uses the same install/update contract above; see :doc:`integration` for user-gesture serial permission and bundler requirements.

Immutable release snapshots and single-BIN updates (0.4.0)
----------------------------------------------------------

SDK 0.5.1 includes these release readers. Import ``loadOfficialFirmware`` and ``loadReleaseManifest`` from ``@esp-vision/device-core/firmware``. The root still exports only ``createDevice``.

``loadOfficialFirmware({ board?, version?, fetch = globalThis.fetch, signal? })`` prioritizes ``config.toml``'s ``[publication]``: ``schemaVersion: 1``, ``version``, 40-character ``sourceCommit``, ``manifestUrl``, raw-byte ``manifestSha256``, and ``releases: [{ version, manifestUrl, manifestSha256, boards }]``. The history has no duplicate versions and each board set is nonempty and unique. Additional publisher transaction fields are preserved but do not change reader behavior. A selected board/version must exist in that reference. Only the selected manifest is downloaded, never the historical board × version matrix; ``firmware.check({ version })`` forwards its version to this reader. An explicit invalid publication/reference/manifest rejects, including a 404 or hash mismatch, without fallback. Only configs lacking ``publication`` use the legacy feed. Explicit versions also restrict legacy metadata requests.

``loadReleaseManifest({ url, sha256, version?, sourceCommit?, boards?, fetch = globalThis.fetch, signal? })`` reads one referenced snapshot without consulting mutable config. SHA-256 covers the original UTF-8 JSON bytes. The result has ``schemaVersion: 1``, ``version``, ``sourceCommit``, positive ``pipelineId``, ``layout: 'legacy' | 'single-bin'``, the existing qualified ``releases`` array, the complete ``website`` snapshot and optional ``mcp`` (null means explicitly absent). Optional identity/board arguments must exactly match; every release and source is validated before returning. Official URLs stay under ``https://dl.espressif.com/AE/esp-vision/``; model downloads use ``raw.githubusercontent.com/espressif/esp-vision/<sourceCommit>/models/...``. MCP descriptors contain ``url/bytes/sha256/version/sourceCommit`` and use the official ``bin/mcp/`` source. Website release/boards/models are checked; hidden firmware boards need not appear in its visual board list. The reader enforces at most 64 release boards, 256 version/model records and 4 MiB of manifest bytes. Hash pinning detects drift from the trusted publication reference; it is not a signature/authentication scheme for an untrusted publisher.

``loadOfficialFirmware`` returns the manifest snapshot plus ``publication``; optional ``board`` filters only returned firmware releases after validation of the complete manifest. Its ``publication.releases`` supplies qualified historical version/board references. ``website.releases`` may retain older downloadable versions without qualified metadata; never advertise those as flashable merely because a BIN URL can be formed. A website built from a fixed reference should pass that reference directly to ``loadReleaseManifest`` when selecting a version, keeping UI and firmware choices tied to one snapshot.

In ``layout: 'single-bin'`` every release has ``image.source: { url, bytes, sha256, offset }``. Its URL equals ``image.url`` and the complete ``install.url``; length/hash equal ``install.bytes/sha256``. ``source.offset`` equals ``image.offset`` and bounds the application inside that complete container. ``image.bytes/sha256`` continue to describe the application. Core downloads the complete source once, rejects size/hash/embedded bootloader/partition/application mismatch before ROM acquisition, extracts the application, then uses the existing device security/layout/write/full-readback flow. A changed partition layout still rejects default update; there is no automatic install fallback. Explicit ``mode: 'install'`` writes/verifies the complete image. Supplied ``data`` follows the selected descriptor: the full container when ``image.source`` is present, otherwise the existing raw application BIN. ``legacy`` manifests and old application-only catalogs remain supported. Downloads are bounded by approved exact size and 256 MiB, use HTTPS, reject redirects and do not use HTTP Range. Browser callers do not download, slice or reimplement verification themselves.

Hash-pinned manifests and complete-BIN sources use browser HTTP cache (``cache: default``); mutable config and legacy app-only BINs retain ``no-store``. A fetch-stage TypeError on a pinned asset gets one ``no-store`` retry for cached responses lacking CORS headers. HTTP, body-read, SHA-256 and metadata failures are not retried or downgraded.

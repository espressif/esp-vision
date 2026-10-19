Communication acceptance, closed-loop tests and troubleshooting
===============================================================

:link_to_translation:`zh_CN:[中文]`

Four levels of acceptance evidence
----------------------------------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     link [label = "1. physical link opened"];
     owner [label = "2. correlated acquire\ncurrent owner / limits"];
     command [label = "3. accepted / queued / complete ACK"];
     app [label = "4. application evidence\nnew marker / file hash / JPEG"];
     link -> owner -> command -> app;
   }

**Figure 24: Lower-level success does not replace application evidence.** Opening serial, acquiring ownership, upload ACK and an actual camera image establish four different conclusions.

A responsive control plane is insufficient to report files/scripts/preview or a complete hardware matrix passed. Record board/physical identity, installed firmware commit, endpoint, session/epoch, request stage, budgets and application evidence.

Suggested acceptance matrix
---------------------------

.. list-table:: Verify functions and boundaries
   :header-rows: 1
   :widths: 26 44 30

   * - Category
     - Operation
     - Success evidence
   * - framing
     - Fragmented metadata/payload, coalesced frames, bad CRC, SOF/EOF inside payload and UTF-8 across frames.
     - Only complete valid frames delivered, bounded resynchronization and no delimiter truncation.
   * - Session
     - Two contenders, same-nonce retries, release/reacquire, lease expiry and TCP slot reuse.
     - One owner, correct errors and no old-link/session input/output on new connections.
   * - VM
     - Busy scripts, Ctrl-C, soft reset, new-epoch readiness and delayed old output.
     - Actual new REPL marker; old epoch cannot complete new requests.
   * - Files
     - Empty, Unicode/binary, chunked/64 KiB, cancellation and unknown publication.
     - Verify bytes/size/content hash and preserve originals/protected files.
   * - preview
     - Continuous JPEG, slow readers, serial switching/reconnection and interleaved RPC.
     - Complete correctly sized/current-scope images; drops allowed, stale-owner data rejected.
   * - Network
     - Configuration accepted/final IP, disable/restore, FIN/RST and silent clients.
     - Final transaction state, distinct owner/slot reclamation and recoverable new connections without replay.
   * - Download/reset
     - Python/USB-control requests, ROM enumeration, write/readback and runtime acquisition.
     - Separately prove mode transition, image verification and new runtime identity, not mere port loss.

Record actually installed firmware, distinguishing source inference, CI compilation and tests on another board from this board acceptance. Browser bridge tests must state whether native serial chooser/DTR were covered.

Unified Core hardware checks
----------------------------

Protocol and application hardware tests use the single ``npm run test:hardware`` entry point in ``host/device-core``, calling the public ``createDevice()`` API. Camera, file and LCD checks share Core rather than maintaining independent host protocol clients or an Agent smoke entry point. The serial adapter only carries bytes, while TCP uses Node sockets; no Agent installation is needed.

Use Node.js 22. Serial mode needs Python with ``pyserial``; ``--python`` selects its interpreter. Keep all outputs inside the project directory. Run these commands from the repository root:

.. code-block:: console

   npm --prefix host/device-core test
   npm --prefix host/device-core run check:schema
   npm --prefix host/device-core run check:package
   npm --prefix host/device-core run test:hardware -- --serial <USJ-port> --role usj --checks all --timeout 600000
   npm --prefix host/device-core run test:hardware -- --serial <USB-OTG-port> --role usb --checks identity,camera,display
   npm --prefix host/device-core run test:hardware -- --tcp <device-ipv4>:3232 --checks identity,memory,camera

``--checks`` accepts ``all`` or comma-separated check names. Existing camera/display checks remain: ``identity``, ``memory``, ``camera``, ``framesizes``, ``pixformats``, ``orientation``, ``ipa_stats``, ``snapshot_rate``, ``preview``, ``image_ops``, ``display`` and ``display_stream``. ``all`` selects the full suite, not a promise that every board has the required hardware. Reports must distinguish passes, failures and unsupported-capability skips; a skip is not a pass. During experimentation test only the current hardware; run the full firmware matrix in CI for final submission.

CI target-test contract
~~~~~~~~~~~~~~~~~~~~~~~

The current CI hardware gate runs all twelve runtime checks on the P4 Function EV Board with ESP-IDF v5.5.6. CI retains both the camera JPEG and the LCD test-pattern JPEG, with dimensions validated against the live display. All selected checks must pass (``--require-all``); an unsupported-capability skip fails this board's gate. Camera-mode loops tolerate only explicit unsupported errors; allocation failures, timeouts and unexpected mode-switch errors fail the check. This proves the selected board/IDF pair, not the other boards in the compilation matrix. The driver passes ``--expect-board`` and the exact ``project_version`` from the downloaded build's ``project_description.json`` to both startup probes and the full suite, rejecting the wrong board or stale firmware before Python/camera/display tests.

Runtime fixtures load into an isolated RAM namespace using bounded ``python.execute()`` calls. Each encoded command fits the firmware's advertised ``replInputMax`` (at most 511 bytes); a larger request would otherwise use Device Core's temporary-file execution path. The fixture namespace and LCD resources are released after success, failure or cancellation. Memory, camera and display checks therefore do not require a writable filesystem.

Storage is separate evidence. The identity check reports volume presence/mount/writer state; a runtime pass does not prove file upload works. Use ``--write`` on a provisioned board to require mounted writable Flash, perform a unique binary file write/readback and remove only the test's own files. Unmounted or unavailable storage fails that explicit check with ``STORAGE_UNAVAILABLE``. Neither CI nor the hardware CLI formats storage, erases data to hide a mount failure, or substitutes an SD volume for Flash.

Camera closed-loop evidence requires a new complete stdout marker or validated JPEG in the current session. Core handles upload, execution and correlation. Checks still cover pixel formats, frame sizes, orientation, IPA state, snapshot rate, preview, image operations and display. Handshake, upload ACK, queued execution or a REPL prompt alone do not prove application completion. Keep receiving during long operations and handle ``VM_BUSY`` / ``VM_TIMEOUT``.

LCD screenshots and test patterns
---------------------------------

``--screen-output`` displays a known test pattern, calls ``device.capture({ source: 'screen', format: 'jpeg' })`` to retrieve a JPEG of the last LCD RGB565 frame, and saves it to the requested path. Create the project-local output directory first:

.. code-block:: console

   mkdir -p /home/libo/project/.esp-vision/hardware-validation
   npm --prefix host/device-core run test:hardware -- --serial <USJ-port> --role usj --checks display --screen-output /home/libo/project/.esp-vision/hardware-validation/lcd.jpg

Capture invokes firmware ``debug.capture_screen`` in the VM task and reads the display buffer without camera calls. Verify JPEG decoding, dimensions and expected pattern, then compare with the actual device screen. An uninitialized display returns ``DISPLAY_NOT_READY``; absent LCD or unsupported firmware returns ``NOT_SUPPORTED``. Never substitute camera capture to hide failure. The screenshot represents the last software-submitted display frame; it does not prove physical panel, cable or backlight operation.

Checks stop current Python programs and may change display content. Schedule exclusive device use rather than sharing ownership with an IDE or another test. Release a valid owner or wait for its lease; switching interfaces does not bypass it. Runtime tests clean their RAM namespace; explicit file tests use unique temporary files and clean their own files. Save reports, JPEGs and logs with actual installed firmware, board and interface. Preserve unknown business completion after reset, disconnection or cancellation instead of replaying writes.

Test entry points and responsibilities
--------------------------------------

.. list-table:: One protocol implementation, layered validation
   :header-rows: 1
   :widths: 40 60

   * - Entry point
     - Coverage
   * - ``host/device-core: npm test``
     - Deterministic host tests of public API, protocol, sessions, discovery and flashing contracts.
   * - ``host/device-core: npm run test:hardware``
     - Real serial/TCP, camera and display closed loops through the same public Core.
   * - ``tools/ci/flash_and_test.py``
     - CI board/IDF artifact selection and flashing followed by Core hardware checks; no separate EV-MUX implementation.
   * - ``tools/tests/``
     - Firmware native-host state-machine, JSON, queue, storage and reset contracts.

Troubleshooting sequence
------------------------

.. list-table:: Symptoms and next checks
   :header-rows: 1
   :widths: 30 70

   * - Symptom
     - Check first
   * - Serial text but hello timeout
     - Raw startup/panic output, wrong USB interface, startup partial frame, runtime mux disablement, CRC/byte lengths and continuous reader.
   * - Configured USB cannot acquire
     - Capabilities and valid owner; DTR=false does not mean v4 unavailable.
   * - ``SESSION_BUSY``
     - Which IDE/Agent/terminal is renewing; release or wait for lease rather than changing routes to force acquisition.
   * - vmReady=true but file/capture timeout
     - Running boot/main.py, busy VM queue and actual new REPL marker after interrupt.
   * - Same-seq data but RPC incomplete
     - Matching type/channel/method/session/epoch; event must not substitute for rsp.
   * - Preview occasionally skips frames
     - Complete current-scope JPEG, slow readers and expected latest-frame behavior.
   * - Both TCP slots exhausted after disconnect
     - Separate handshake/lease/30-second idle states and reclaimed old generations, not just released owner.
   * - Ports disappear after download request
     - ROM re-enumeration, ROM output on USJ, masked download-bit setting/clearing and invalidated handles.
   * - Global drops grow but TCP marker passes
     - Unread default/fallback interfaces, queue classes and sample scope; other-endpoint counters do not equal this business failure.

Store only necessary state/scalar counters. SSID/password, file content and Flash/NVS backups can contain sensitive information and should not enter public reports. Placeholder examples are not actual configuration.

Keeping documentation aligned with implementation
-------------------------------------------------

New channels/backends/actions or stderr/ownership/limit changes require updates to related pages, capabilities, Core and contract tests. Renaming diagram labels alone is not a protocol change; methods need real firmware dispatch implementations.

Firmware unit tests and Core acceptance
---------------------------------------

``tools/tests/test_ev_*.py`` compiles/runs production C logic on the host with ESP-IDF/FreeRTOS fakes; it does not connect to a device or implement another host protocol client. Keep deterministic coverage of partial writes, queue pressure, allocation failures, stale connection generations, session expiry and storage handoff. The accompanying C harnesses and stub headers are test infrastructure, not alternative device-operation tools.

Device Core owns application-visible and hardware acceptance tests. Use its public API for new connection, Python, file, preview and capture scenarios. Firmware fault injection stays alongside firmware because a normal SDK request cannot reliably force task scheduling, memory allocation or driver failures. Source-text assertions are supplementary checks, not substitutes for executing behavior or real hardware acceptance.

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

Use Node.js 22. Serial mode uses npm native bindings; no host Python is required. Keep all outputs inside the project directory. Run these commands from the repository root:

.. code-block:: console

   npm --prefix host/device-core test
   npm --prefix host/device-core run check:schema
   npm --prefix host/device-core run check:package
   npm --prefix host/device-core run test:hardware -- --serial <USJ-port> --role usj --checks all --timeout 600000
   npm --prefix host/device-core run test:hardware -- --serial <USB-OTG-port> --role usb --checks identity,camera,display
   npm --prefix host/device-core run test:hardware -- --tcp <device-ipv4>:3232 --checks identity,memory,camera

``--checks`` accepts ``all`` or comma-separated check names. Camera/display checks include ``identity``, ``memory``, ``camera``, ``framesizes``, ``pixformats``, ``orientation``, ``ipa_stats``, ``snapshot_rate``, ``preview``, ``image_ops``, ``display``, ``display_stream``, ``camera_display_lifecycle``, ``image_copy_to_fb``, ``image_transform_argument``, ``sensor_snapshot_buffer``, ``memory_policy``, ``espdl_inference``, ``isp_ccm_frames`` and ``isp_lsc_display``. ``all`` selects the full suite, not a promise that every board has the required hardware. Reports must distinguish passes, failures and unsupported-capability skips; a skip is not a pass. During experimentation test only the current hardware; run the full firmware matrix in CI for final submission.

ISP image and allocation checks
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use a stationary illuminated target and select the exact board and installed firmware version for an ISP investigation:

.. code-block:: console

   npm --prefix host/device-core run test:hardware -- --serial <USB-OTG-port> --role usb --checks isp_ccm_frames,isp_lsc_display --expect-board ESP32_P4X_EYE --expect-firmware-version <installed-version> --frames 400 --require-all

``isp_ccm_frames`` writes each controlled red/grey CCM matrix once and records eight consecutive rendered-frame samples with frame numbers, elapsed time, RGB sums and mean saturation. It deliberately includes a two-second sleep before one grey sequence: the first buffer may still be old, while continued consumption must reveal the new output within the bounded sequence. It also verifies that IPA rejects manual writes and that rewriting an identity CCM preserves explicit ISP white-balance gains in both readback and rendered RGB ratios. These ratios require a stationary target; they do not measure or freeze the sensor's internal automatic white balance. Fewer than sixteen sufficiently bright samples fails the check as unobservable, rather than treating a dark frame as proof that CCM works.

``isp_lsc_display`` uses QVGA RGB565, the reported LSC gain-table size, the report's 1.0-to-0.5 quadratic ramp, and a centered camera image drawn onto a full-panel canvas before ``lcd.write()``. After warming both LSC table banks and display staging, it runs at least eighty frames (``--frames`` raises that count), writes CCM twice per frame, and toggles LSC every twenty frames. Reports include free and largest-block DMA/internal/PSRAM heaps, invalid-table rejection followed by a successful display write, resource release and a fresh camera/LCD lifetime. Heap history retains at most ten samples so observation storage stays bounded. A bounded heap tolerance allows small unrelated allocations; inspect the recorded series for trends during longer runs. This is software-frame and resource evidence. Compare the actual LCD while LSC switches to establish whether the physical panel is free of corruption; a passing check or software screenshot cannot establish that alone. A pass on current firmware does not retroactively identify the root cause of an older firmware report.

CI target-test contract
~~~~~~~~~~~~~~~~~~~~~~~

The CI hardware gate runs eighteen explicitly selected runtime checks, including ``espdl_inference``, on the P4 Function EV Board with ESP-IDF v5.5.6. The two ISP checks remain separate manual acceptance on an illuminated stationary target; adding a registered check does not silently expand the unattended CI suite. CI retains both the camera JPEG and the LCD test-pattern JPEG, with dimensions validated against the live display. All selected checks must pass (``--require-all``); an unsupported-capability skip fails this board's gate. Camera-mode loops tolerate only explicit unsupported errors; allocation failures, timeouts and unexpected mode-switch errors fail the check. This proves the selected board/IDF pair, not the other boards in the compilation matrix. The driver passes ``--expect-board`` and the exact ``project_version`` from the downloaded build's ``project_description.json`` to both startup probes and the full suite, rejecting the wrong board or stale firmware before Python/camera/display tests.

To qualify one board through CI, set ``ESP_VISION_TEST_BOARD`` and ``ESP_VISION_TEST_IDF_IMAGE`` to its exact firmware matrix pair, and explicitly set ``ESP_VISION_TEST_HARDWARE=1``. For P4 Function EV Board use ``ESP32_P4X_FUNCTION_EV_BOARD`` and ``v5.5.6``. Only the matching firmware and hardware jobs are selected; without the hardware opt-in, a targeted run only builds firmware. ``ESP_VISION_DEVICE_CORE_ONLY=1`` still disables all firmware and hardware jobs.

Runtime fixtures load into an isolated RAM namespace using bounded ``python.execute()`` calls. Each encoded command fits the firmware's advertised ``replInputMax`` (at most 511 bytes); a larger request would otherwise use Device Core's temporary-file execution path. The fixture namespace and LCD resources are released after success, failure or cancellation. Memory, camera and display checks therefore do not require a writable filesystem.

Fixture source transfer uses UTF-8 byte offsets and acknowledgements containing the session nonce, offset and committed content. Repeating an identical chunk is safe; a conflicting chunk or invalid acknowledgement fails immediately. The loader verifies the assembled byte count and SHA-256 before installation, then requires an acknowledgement of completed installation, callable entry points and released source buffers. Only ``TIMEOUT`` permits recovery, with at most three attempts per bootstrap operation through the public Python API and unchanged per-call/overall deadlines. A lost response after completed installation can recover without re-executing the fixture; an interrupted, partially completed installation fails and is never replayed. Cancellation and semantic errors are not retried.

Bootstrap retries and failures report the phase (``init``, ``chunk``, ``assemble``, ``verify`` or ``install``), zero-based command index, attempt, encoded input bytes, elapsed milliseconds and last acknowledged operation; chunk diagnostics also include zero-based chunk index, UTF-8 offset and byte count. Capability inspection and host source-read failures are labelled separately. A public execution timeout can include SDK synchronization as well as command execution; these diagnostics locate the bootstrap operation but do not establish a transport or firmware root cause. Reports contain no raw Python output or source text.

An unavailable fixture produces one ``fixture-bootstrap`` failure. Every dependent check, including the first affected check, reports ``BLOCKED`` with ``blockedBy: fixture-bootstrap`` and ``FIXTURE_UNAVAILABLE``; independent identity checks still run. The summary counts ``passed``, ``failed``, ``skipped`` and ``blocked`` separately. Blocked checks are neither executed failures nor unsupported-capability skips, and the hardware job fails even without ``--require-all``. Cleanup uses an independent cancellation signal; a cleanup failure is reported separately and does not replace the original bootstrap failure or cancellation.

The ``espdl_inference`` check transfers the repository's 495,840-byte ESPDet Pico Face model through bounded public ``python.execute()`` calls into a read-only RAM VFS, verifies SHA-256 on both host and device, and invokes the real ``espdl.ESPDet`` on RGB565 camera frames. It checks repeatability on the same copied frame, detection tuple shape, box overlap, confidence/class ranges and positive inference timing. Reports include the model SHA-256, transfer chunk count/time, load time, frame/detection counts, and mean/worst inference latency. An empty detection list is valid when no face is visible; this sanity check does not establish model accuracy or a latency target. The model, RAM mount and camera are released after success, failure or cancellation; no Flash/SD files or MSC writer leases are used.

Model uploads acknowledge a namespace nonce and exact committed byte offsets. Only a timed-out block may be retried, with at most three public execution attempts; replay accepts only the latest identical block. Conflicting/out-of-order data, missing or incorrect acknowledgements, and cancellation fail immediately. Reports include ``modelRetries`` and ``modelRetryOffsets``. Mounting, model cleanup and final namespace absence require explicit acknowledgements/readback, including when Python prints an exception but the execution wrapper returns success. Existing per-command and overall deadlines remain unchanged.

The default model SHA-256 is ``04eec2220adbf642a843c953817fb219a06c094aedb76c181ba2a958c65201dc``. ``--model MODEL.espdl`` selects another compatible ESPDet model (at most 2 MiB); add ``--model-sha256 HEX`` to pin its expected bytes. Installed npm packages do not bundle model assets: supply ``--model`` there. A missing model skips optional acceptance but fails ``--require-all``. Camera/ESP-DL support is required for strict acceptance, independently of persistent storage availability.

Storage is separate evidence. The identity check reports volume presence/mount/writer state; a runtime pass does not prove file upload works. Use ``--write`` on a provisioned board to require mounted writable Flash, perform a unique binary file write/readback and remove only the test's own files. Unmounted or unavailable storage fails that explicit check with ``STORAGE_UNAVAILABLE``. Neither CI nor the hardware CLI formats storage, erases data to hide a mount failure, or substitutes an SD volume for Flash.

Camera closed-loop evidence requires a new complete stdout marker or validated JPEG in the current session. Core handles upload, execution and correlation. Checks still cover pixel formats, frame sizes, orientation, IPA state, snapshot rate, preview, image operations and display. Handshake, upload ACK, queued execution or a REPL prompt alone do not prove application completion. Keep receiving during long operations and handle ``VM_BUSY`` / ``VM_TIMEOUT``.

LCD screenshots and test patterns
---------------------------------

``--screen-output`` displays a known test pattern, calls ``device.capture({ source: 'screen', format: 'jpeg' })`` to retrieve a JPEG of the last LCD RGB565 frame, and saves it to the requested path. Set ``ESP_VISION_WORK_DIR`` to a writable workspace outside the checkout, then create the output directory:

.. code-block:: console

   mkdir -p "$ESP_VISION_WORK_DIR/hardware-validation"
   npm --prefix host/device-core run test:hardware -- --serial <USJ-port> --role usj --checks display --screen-output "$ESP_VISION_WORK_DIR/hardware-validation/lcd.jpg"

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


11.14 release target acceptance
-------------------------------

The release target jobs for Function EV, P4 Vision and S31 Korvo now require the following checks in addition to their board-specific camera, image, display and inference cases. Unsupported selected checks fail with ``--require-all``; a missing board or peripheral is not a passing result.

* ``--verify-flash`` independently compares every image in ``flasher_args.json`` against Flash using esptool, including the same image-header settings used when writing. A failed comparison prevents runtime tests. Multiple serial candidates require an explicit ``ESPPORT``/``--port`` before any write; the selected port is retained for verification and runtime acquisition.
* ``discovery`` enumerates the selected serial endpoint through passive SDK inventory while a nonce-scoped Python variable is live, then verifies the variable and runtime identity remain intact. On an explicitly selected TCP endpoint it instead requires a matching mDNS advertisement and device ID. Enumeration never probes other devices. The standard serial CI jobs do not establish LAN discovery coverage.
* ``communication`` checks five distinct Unicode output streams, each larger than 4 KiB in UTF-8, with an identity/runtime RPC between executions. Truncated, stale or altered output fails.
* ``reconnect`` releases and reacquires the selected transport three times, checking device ID, board, firmware version and a new Python marker each time. This tests orderly disconnect/reconnect, not cable removal, power loss or lease contention.
* ``modules`` executes known-answer SHA-256, hex/base64 binary roundtrips, struct byte ordering, Unicode JSON, BytesIO seek/read and garbage collection. ``ulab`` computes and checks mean, elementwise multiplication/sum, square root and sorting on the target. Both use RAM without requiring Flash/SD writes.

``--junit PATH`` on the Python driver produces GitLab test cases for the individual SDK checks, flash/verify stages and runtime readiness. Bootstrap progress events remain diagnostic log entries; blocked checks appear as failures with their prerequisite recorded. Failures before runtime acceptance produce a failing driver case. Logs and camera/screen artifacts remain available; a successful flash stage alone does not qualify a release.

For the 2026.11.14 candidate, retain the final commit/pipeline, board, interface, firmware version and image artifacts together. Run the full CI firmware matrix only after implementation is complete. The existing publication gate still requires all three target jobs. Tests on an older installed firmware validate the harness only; they do not qualify the candidate. Tag creation and publication are separate release actions.

For coordinated npm releases, CLI packaging on protected master requires successful Core publication or confirmed availability of that version. It resolves the published archive and SHA-512 integrity before ``npm ci``, builds and tests the CLI, then publishes that exact checked tarball with the resolved lock. MR checks may use the matching source Core tarball while publication is pending. Plugin release packaging requires the published CLI to bundle the Core version recorded in the Plugin manifest; it does not fall back to an older CLI or Core. Core 0.8.1 must therefore be available before CLI 0.2.1, and both must be available before packaging Plugin 0.2.0. Firmware qualification, npm publication and online Knowledge availability are separate acceptance milestones; a firmware tag alone does not prove that its Knowledge is online.

Core 0.8.1, CLI 0.2.1, Plugin 0.2.0 and Knowledge MCP 0.2.0 are already published to npm. Knowledge MCP 0.2.0 is independent of Core/CLI pins. Its initial retained catalog covers 2026.10.21, 2026.10.23 and 2026.11.06; it does not yet cover the 2026.11.14 firmware candidate. Publish the new firmware snapshot after the tag qualifies and its binaries ship, then import it through a separate reviewed Knowledge release. The Plugin 0.2.0 publication gate waits for any concurrent Knowledge deployment and verifies the live versioned endpoint before writing to npm.
